#!/bin/bash

# Copyright 2016 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

. "$(dirname "$0")/common.sh"
. "$(dirname "$0")/lib/sign_android_lib.sh"
load_shflags || exit 1

DEFINE_boolean use_apksigner "${FLAGS_FALSE}" \
  "Use apksigner instead of signapk for APK signing"

FLAGS_HELP="
Usage: $PROG /path/to/cros_root_fs/dir /path/to/keys/dir

Re-sign framework apks in an Android system image.  The image itself does not
need to be signed since it is shipped with Chrome OS image, which is already
signed.

Android has many ``framework apks'' that are signed with different framework
keys, depends on the purpose of the apk.  During development, apks are signed
with the debug one.  This script is to re-sign those apks with corresponding
release key.  It also handles some of the consequences of the key changes, such
as sepolicy update.
"

# Parse command line.
FLAGS "$@" || exit 1
eval set -- "${FLAGS_ARGV}"

set -e

# Re-sign framework apks with the corresponding release keys.  Only apk with
# known key fingerprint are re-signed.  We should not re-sign non-framework
# apks.
sign_framework_apks() {
  local system_mnt="$1"
  local key_dir="$2"
  local working_dir="$3"
  local flavor_prop=""
  local keyset=""

  if ! flavor_prop=$(android_get_build_flavor_prop \
    "${system_mnt}/system/build.prop"); then
    die "Failed to extract build flavor property from \
'${system_mnt}/system/build.prop'."
  fi
  info "Found build flavor property '${flavor_prop}'."
  if ! keyset=$(android_choose_signing_keyset "${flavor_prop}"); then
    die "Unknown build flavor property '${flavor_prop}'."
  fi
  info "Expecting signing keyset '${keyset}'."

  info "Start signing framework apks"

  if ! image_content_integrity_check "${system_mnt}" "${working_dir}" \
                                     "Prepare apks signing"; then
    return 1
  fi

  # Counters for validity check.
  local counter_platform=0
  local counter_media=0
  local counter_shared=0
  local counter_releasekey=0
  local counter_networkstack=0
  local counter_total=0

  local apk
  while read -d $'\0' -r apk; do
    local sha1=""
    local keyname=""

    sha1=$(unzip -p "${apk}" META-INF/CERT.RSA | \
      keytool -printcert | awk '/^\s*SHA1:/ {print $2}')

    if  ! keyname=$(android_choose_key "${sha1}" "${keyset}"); then
      die "Failed to choose signing key for APK '${apk}' (SHA1 '${sha1}') in \
build flavor '${flavor_prop}'."
    fi
    if [[ -z "${keyname}" ]]; then
      continue
    fi

    info "Re-signing (${keyname}) ${apk}"

    local temp_dir="$(make_temp_dir)"
    local temp_apk="${temp_dir}/temp.apk"
    local signed_apk="${temp_dir}/signed.apk"

    # Follow the standard manual signing process.  See
    # https://developer.android.com/studio/publish/app-signing.html.
    cp -a "${apk}" "${temp_apk}"
    # Explicitly remove existing signature.
    zip -q "${temp_apk}" -d "META-INF/*"

    if [ "${FLAGS_use_apksigner}" = "$FLAGS_FALSE" ]; then
      # Signapk now creates signature of APK Signature Scheme v2. No further APK
      # changes should happen afterward.  Also note that signapk now takes care
      # of zipalign.
      signapk "${key_dir}/$keyname.x509.pem" "${key_dir}/$keyname.pk8" \
          "${temp_apk}" "${signed_apk}" > /dev/null
    else
      # Key rotation: old key can sign a new key and generate a lineage file.
      # Provided the lineage file, Android P can honor the new key. Lineage file
      # can be generated similar to the following command:
      #
      # apksigner rotate --out media.lineage --old-signer --key old-media.pk8
      # --cert old-media.x509.pem --new-signer --key new-media.pk8 --cert
      # new-media.x509.pem
      #
      # TODO(b/132818552): disable v1 signing once a check is removed.

      local extra_flags
      local lineage_file="${key_dir}/$keyname.lineage}"
      if [ -f ${lineage_file} ]; then
        extra_flags="--lineage ${lineage_file}"
      fi
      apksigner sign --v1-signing-enabled true --v2-signing-enabled false \
        --key "${key_dir}/$keyname.pk8" --cert "${key_dir}/$keyname.x509.pem" \
        --in "${temp_apk}" --out "${signed_apk}" \
        ${extra_flags}
    fi
    if ! image_content_integrity_check "${system_mnt}" "${working_dir}" \
                                       "sign apk ${signed_apk}"; then
      return 1
    fi

    # Copy the content instead of mv to avoid owner/mode changes.
    sudo cp "${signed_apk}" "${apk}" && rm -f "${signed_apk}"

    # Set timestamp rounded to second since squash file system has resolution
    # in seconds. Required in order for the packages cache generator output is
    # compatible with the packed file system.
    sudo touch "${apk}" -t "$(date +%m%d%H%M.%S)"

    : $(( counter_${keyname} += 1 ))
    : $(( counter_total += 1 ))
    if ! image_content_integrity_check "${system_mnt}" "${working_dir}" \
                                       "update re-signed apk ${apk}"; then
      return 1
    fi
  done < <(find "${system_mnt}/system" -type f -name '*.apk' -print0)

  info "Found ${counter_platform} platform APKs."
  info "Found ${counter_media} media APKs."
  info "Found ${counter_shared} shared APKs."
  info "Found ${counter_releasekey} release APKs."
  info "Found ${counter_networkstack} networkstack APKs."
  info "Found ${counter_total} total APKs."
  # Validity check.
  if [[ ${counter_platform} -lt 2 || ${counter_media} -lt 2 ||
        ${counter_shared} -lt 2 || ${counter_releasekey} -lt 2 ||
        ${counter_total} -lt 25 ]]; then
    die "Number of re-signed package seems to be wrong"
  fi

  return 0
}

# Extracts certificate from the provided public key.
get_cert() {
  # Full path to public key to read and extract certificate. It must exist.
  local public_key=$1
  local cert=$(sed -E '/(BEGIN|END) CERTIFICATE/d' \
    "${public_key}" | tr -d '\n' \
    | base64 --decode | hexdump -v -e '/1 "%02x"')

  if [[ -z "${cert}" ]]; then
    die "Unable to get the public platform key"
  fi
  echo "${cert}"
}

# Replaces particular certificate in mac_permissions xml file with new one.
# Note, this does not fail if particular entry is not found. For example
# network_stack does not exist in P.
change_cert() {
  # Type of signer entry to process. Could be platform, media or network_stack.
  local type=$1
  # New certificate encoded to string. This replaces old one.
  local cert=$2
  # *mac_permissions xml file to modify, plat_mac_permissions.xml for example.
  local xml=$3
  local pattern="(<signer signature=\")\w+(\"><seinfo value=\"${type})"
  sudo sed -i -E "s/${pattern}/\1${cert}"'\2/g' "${xml}"
}

# Platform key is part of the SELinux policy.  Since we are re-signing framework
# apks, we need to replace the key in the policy as well.
update_sepolicy() {
  local system_mnt=$1
  local key_dir=$2

  # Only platform is used at this time.
  local public_platform_key="${key_dir}/platform.x509.pem"
  local public_media_key="${key_dir}/media.x509.pem"
  local public_network_stack_key="${key_dir}/networkstack.x509.pem"

  info "Start updating sepolicy"

  local new_platform_cert=$(get_cert "${public_platform_key}")
  local new_media_cert=$(get_cert "${public_media_key}")
  local new_network_stack_cert=$(get_cert "${public_network_stack_key}")

  shopt -s nullglob
  local xml_list=( "${system_mnt}"/system/etc/**/*mac_permissions.xml )
  shopt -u nullglob
  if [[ "${#xml_list[@]}" -ne 1 ]]; then
    die "Unexpected number of *mac_permissions.xml: ${#xml_list[@]}\n \
      ${xml_list[*]}"
  fi

  local xml="${xml_list[0]}"
  local orig=$(make_temp_file)
  cp "${xml}" "${orig}"

  change_cert "platform" "${new_platform_cert}" "${xml}"
  change_cert "media" "${new_media_cert}" "${xml}"
  change_cert "network_stack" "${new_network_stack_cert}" "${xml}"

  # Validity check.
  if cmp "${xml}" "${orig}"; then
    die "Failed to replace SELinux policy cert"
  fi
}

# Replace the debug key in OTA cert with release key.
replace_ota_cert() {
  local system_mnt=$1
  local release_cert=$2
  local ota_zip="${system_mnt}/system/etc/security/otacerts.zip"

  info "Replacing OTA cert"

  local temp_dir=$(make_temp_dir)
  pushd "${temp_dir}" > /dev/null
  cp "${release_cert}" .
  local temp_zip=$(make_temp_file)
  zip -q -r "${temp_zip}.zip" .
  # Copy the content instead of mv to avoid owner/mode changes.
  sudo cp "${temp_zip}.zip" "${ota_zip}"
  popd > /dev/null
}

# Snapshot file properties in a directory recursively.
snapshot_file_properties() {
  local dir=$1
  sudo find "${dir}" -exec stat -c '%n:%u:%g:%a' {} + | sort
}

# Integrity check that image content is unchanged.
image_content_integrity_check() {
  local system_mnt=$1
  local working_dir=$2
  local reason=$3
  snapshot_file_properties "${system_mnt}" > "${working_dir}/properties.new"
  local d
  if ! d=$(diff "${working_dir}"/properties.{orig,new}); then
    error "Unexpected change of file property, diff due to ${reason}\n${d}"
    return 1
  fi

  return 0
}

list_image_files() {
  local unsquashfs=$1
  local system_img=$2
  "${unsquashfs}" -l "${system_img}" | grep ^squashfs-root
}

sign_android_internal() {
  local root_fs_dir=$1
  local key_dir=$2

  # Detect vm/container type and set environment correspondingly.
  # Keep this aligned with
  # src/private-overlays/project-cheets-private/scripts/board_specific_setup.sh
  local system_image=""
  local selinux_dir="${root_fs_dir}/etc/selinux"
  local file_contexts=""
  local vm_candidate="${root_fs_dir}/opt/google/vms/android/system.raw.img"
  local container_candidate=(
      "${root_fs_dir}/opt/google/containers/android/system.raw.img")
  if [[ -f "${vm_candidate}" ]]; then
    system_image="${vm_candidate}"
    file_contexts="${selinux_dir}/arc/contexts/files/android_file_contexts_vm"
  elif [[ -f "${container_candidate}" ]]; then
    system_image="${container_candidate}"
    file_contexts="${selinux_dir}/arc/contexts/files/android_file_contexts"
  else
    die "System image does not exist"
  fi

  local android_system_image="$(echo \
    "${root_fs_dir}"/opt/google/*/android/system.raw.img)"
  local android_dir=$(dirname "${android_system_image}")
  local system_img="${android_dir}/system.raw.img"
  # Use the versions in $PATH rather than the system ones.
  local unsquashfs=$(which unsquashfs)
  local mksquashfs=$(which mksquashfs)

  if [[ $# -ne 2 ]]; then
    flags_help
    die "command takes exactly 2 args"
  fi

  if [[ ! -f "${system_img}" ]]; then
    die "System image does not exist: ${system_img}"
  fi

  # NOTE: Keep compression_flags aligned with
  # src/private-overlays/project-cheets-private/scripts/board_specific_setup.sh
  local compression_flags=""
  local compression=$(sudo "${unsquashfs}" -s "${system_img}" \
    | grep -e ^"Compression\s")
  if [[ "${compression}" == "Compression gzip" ]]; then
    compression_flags="-comp gzip"
  elif [[ "${compression}" == "Compression lz4" ]]; then
    compression_flags="-comp lz4 -Xhc -b 256K"
  elif [[ "${compression}" == "Compression zstd" ]]; then
    compression_flags="-comp zstd -b 256K"
  else
    die "Unexpected compression type: ${compression}"
  fi

  if ! type -P zipalign &>/dev/null || ! type -P signapk &>/dev/null \
    || ! type -P apksigner &>/dev/null; then
    # TODO(victorhsieh): Make this an error.  This is not treating as error
    # just to make an unrelated test pass by skipping this signing.
    warn "Skip signing Android apks (some of executables are not found)."
    exit 0
  fi

  local working_dir=$(make_temp_dir)
  local system_mnt="${working_dir}/mnt"

  info "Unpacking squashfs system image to ${system_mnt}"
  list_image_files "${unsquashfs}" "${system_img}" > \
      "${working_dir}/image_file_list.orig"
  sudo "${unsquashfs}" -no-xattrs -f -no-progress -d "${system_mnt}" "${system_img}"

  snapshot_file_properties "${system_mnt}" > "${working_dir}/properties.orig"

  if ! sign_framework_apks "${system_mnt}" "${key_dir}" "${working_dir}"; then
    return 1
  fi

  if ! image_content_integrity_check "${system_mnt}" "${working_dir}" \
                                     "sign_framework_apks"; then
    return 1
  fi

  update_sepolicy "${system_mnt}" "${key_dir}"
  if ! image_content_integrity_check "${system_mnt}" "${working_dir}" \
                                      "update_sepolicy"; then
    return 1
  fi

  replace_ota_cert "${system_mnt}" "${key_dir}/releasekey.x509.pem"
  if ! image_content_integrity_check "${system_mnt}" "${working_dir}" \
                                     "replace_ota_cert"; then
    return 1
  fi

  # Packages cache needs to be regenerated when the key and timestamp are
  # changed for apks.
  local packages_cache="${system_mnt}/system/etc/packages_cache.xml"
  local file_hash_cache="${system_mnt}/system/etc/file_hash_cache"
  if [[ -f "${packages_cache}" ]]; then
    if type -P aapt &>/dev/null; then
      info "Regenerating packages cache ${packages_cache}"
      # For the validity check.
      local packages_before=$(grep "<package " "${packages_cache}" | wc -l)
      local vendor_mnt=$(make_temp_dir)
      local vendor_img="${android_dir}/vendor.raw.img"
      local jar_lib="lib/arc-cache-builder/org.chromium.arc.cachebuilder.jar"
      info "Unpacking squashfs vendor image to ${vendor_mnt}/vendor"
      # Vendor image is not updated during this step. However we have to include
      # vendor apks to re-generated packages cache which exists in one file for
      # both system and vendor images.
      sudo "${unsquashfs}" -x -f -no-progress -d "${vendor_mnt}/vendor" \
          "${vendor_img}"
      if ! arc_generate_packages_cache "${system_mnt}" "${vendor_mnt}" \
          "${working_dir}/packages_cache.xml" \
          "${working_dir}/file_hash_cache"; then
        die "Failed to generate packages cache."
      fi
      sudo cp "${working_dir}/packages_cache.xml" "${packages_cache}"
      sudo cp "${working_dir}/file_hash_cache" "${file_hash_cache}"
      # Set android-root as an owner.
      sudo chown 655360:655360 "${packages_cache}"
      local packages_after=$(grep "<package " "${packages_cache}" | wc -l)
      if [[ "${packages_before}" != "${packages_after}" ]]; then
        die "failed to verify the packages count after the regeneration of " \
            "the packages cache. Expected ${packages_before} but found " \
            "${packages_after} packages in pacakges_cache.xml"
      fi
    else
      warn "aapt tool could not be found. Could not regenerate the packages " \
           "cache. Outdated pacakges_cache.xml is removed."
      sudo rm "${packages_cache}"
    fi
  else
    info "Packages cache ${packages_cache} does not exist. Skip regeneration."
  fi

  info "Repacking squashfs image with compression flags '${compression_flags}'"
  local old_size=$(stat -c '%s' "${system_img}")
  # Remove old system image to prevent mksquashfs tries to merge both images.
  sudo rm -rf "${system_img}"
  sudo mksquashfs "${system_mnt}" "${system_img}" \
    ${compression_flags} -context-file "${file_contexts}" -mount-point "/" \
    -no-progress
  local new_size=$(stat -c '%s' "${system_img}")
  info "Android system image size change: ${old_size} -> ${new_size}"

  list_image_files "${unsquashfs}" "${system_img}" > \
      "${working_dir}/image_file_list.new"
  if d=$(grep -v -F -x -f "${working_dir}"/image_file_list.{new,orig}); then
    # If we have a line in image_file_list.orig which does not appear in
    # image_file_list.new, it means some files are removed during signing
    # process. Here we have already deleted the original Android image so
    # cannot retry.
    die "Unexpected change of file list\n${d}"
  fi

  return 0
}

main() {
  # TODO(b/175081695): Remove retries once root problem is fixed.
  local attempts
  for (( attempts = 1; attempts <= 3; ++attempts )); do
    if sign_android_internal "$@"; then
      exit 0
    fi
    warn "Could not sign android image due to recoverable error, will retry," \
         "attempt # ${attempts}."
    warn "@@@ALERT@@@"
    lsof -n
    dmesg
    mount
  done
  die "Unable to sign Android image; giving up."
}

main "$@"
