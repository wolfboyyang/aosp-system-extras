/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ext4_utils/ext4_crypt_init_extensions.h"

#include <dirent.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <cutils/properties.h>
#include <cutils/sockets.h>
#include <keyutils.h>
#include <logwrap/logwrap.h>

#include "ext4_utils/ext4_crypt.h"

#define TAG "ext4_utils"

static const std::string arbitrary_sequence_number = "42";
static const int vold_command_timeout_ms = 60 * 1000;

int e4crypt_install_keyring()
{
    key_serial_t device_keyring = add_key("keyring", "e4crypt", 0, 0,
                                          KEY_SPEC_SESSION_KEYRING);

    if (device_keyring == -1) {
        PLOG(ERROR) << "Failed to create keyring";
        return -1;
    }

    LOG(INFO) << "Keyring created with id " << device_keyring << " in process " << getpid();

    return 0;
}

int e4crypt_set_directory_policy(const char* dir)
{
    // Only set policy on first level /data directories
    // To make this less restrictive, consider using a policy file.
    // However this is overkill for as long as the policy is simply
    // to apply a global policy to all /data folders created via makedir
    if (!dir || strncmp(dir, "/data/", 6) || strchr(dir + 6, '/')) {
        return 0;
    }

    // Special case various directories that must not be encrypted,
    // often because their subdirectories must be encrypted.
    // This isn't a nice way to do this, see b/26641735
    std::vector<std::string> directories_to_exclude = {
        "lost+found",
        "system_ce", "system_de",
        "misc_ce", "misc_de",
        "media",
        "data", "user", "user_de",
    };
    std::string prefix = "/data/";
    for (auto d: directories_to_exclude) {
        if ((prefix + d) == dir) {
            LOG(INFO) << "Not setting policy on " << dir;
            return 0;
        }
    }

    std::string ref_filename = std::string("/data") + e4crypt_key_ref;
    std::string policy;
    if (!android::base::ReadFileToString(ref_filename, &policy)) {
        LOG(ERROR) << "Unable to read system policy to set on " << dir;
        return -1;
    }

    auto type_filename = std::string("/data") + e4crypt_key_mode;
    std::string contents_encryption_mode;
    if (!android::base::ReadFileToString(type_filename, &contents_encryption_mode)) {
        LOG(ERROR) << "Cannot read mode";
    }

    LOG(INFO) << "Setting policy on " << dir;
    int result = e4crypt_policy_ensure(dir, policy.c_str(), policy.length(),
                                       contents_encryption_mode.c_str());
    if (result) {
        LOG(ERROR) << android::base::StringPrintf(
            "Setting %02x%02x%02x%02x policy on %s failed!",
            policy[0], policy[1], policy[2], policy[3], dir);
        return -1;
    }

    return 0;
}
