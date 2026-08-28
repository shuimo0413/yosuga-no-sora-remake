#!/usr/bin/env python
# coding=utf-8
##############################################
# Copyright (c) 2021-2022 Huawei Device Co., Ltd.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##############################################
import json
import os
import re
import stat
import sys
from subprocess import Popen
from subprocess import PIPE

global_config = {}

templates = {
    'generate-keypair': {
        'required': ['keyAlias', 'keyAlg', 'keySize', 'keystoreFile'],
        'others': ['keyPwd', 'keystorePwd']
    },
    'generate-csr': {
        'required': ['keyAlias', 'signAlg', 'subject', 'keystoreFile', 'outFile'],
        'others': ['keyPwd', 'keystorePwd']
    },
    'generate-ca': {
        'required': ['keyAlias', 'signAlg', 'keyAlg', 'keySize', 'subject', 'keystoreFile', 'outFile'],
        'others': ['keyPwd', 'keystorePwd', 'issuer', 'issuerKeyAlias', 'issuerKeyPwd', 'validity',
                   'basicConstraintsPathLen']
    },
    'generate-app-cert': {
        'required': ['keyAlias', 'signAlg', 'issuer', 'issuerKeyAlias', 'subject', 'keystoreFile',
                     'subCaCertFile', 'rootCaCertFile', 'outForm', 'outFile'],
        'others': ['keyPwd', 'keystorePwd', 'issuerKeyPwd', 'validity',
                   'basicConstraintsPathLen']
    },
    'generate-profile-cert': {
        'required': ['keyAlias', 'signAlg', 'issuer', 'issuerKeyAlias', 'subject', 'keystoreFile',
                     'subCaCertFile', 'rootCaCertFile', 'outForm', 'outFile'],
        'others': ['keyPwd', 'keystorePwd', 'issuerKeyPwd', 'validity',
                   'basicConstraintsPathLen']
    },
    'sign-profile': {
        'required': ['keyAlias', 'signAlg', 'mode', 'profileCertFile', 'inFile', 'keystoreFile', 'outFile'],
        'others': ['keyPwd', 'keystorePwd']
    },
    'sign-app': {
        'required': ['keyAlias', 'signAlg', 'mode', 'appCertFile', 'profileFile', 'inFile', 'keystoreFile', 'outFile'],
        'others': ['keyPwd', 'keystorePwd', 'inForm', 'signCode']
    },
}


def print_help():
    content = "\n" \
              "Usage:python autosign.py <generate|sign> \n" \
              "    signtool.jar : Main progress jar file\n" \
              "\n" \
              "Example: \n" \
              "    python autosign.py createAppCertAndProfile \n" \
              "    python autosign.py signHap" \
              "\n"
    print(content)


def get_from_single_config(config_key, item_key, required=False):
    param = global_config.get(config_key, {}).get(item_key, None)
    if not param:
        param = global_config.get('common', {}).get(item_key, None)
    if not param:
        if required:
            print('Prepare loading: {}, config: {}'.format(config_key, global_config.get(config_key)))
            print("Params {} is required.".format(item_key))
            exit(1)
    return param


def prepare_dir(dir_name):
    if not os.path.exists(dir_name):
        os.mkdir(dir_name)


def load_engine(engine_config):
    tar_dir = global_config.get('config', {}).get('targetDir')
    prepare_dir(tar_dir)

    cmds = []
    for eng_k, eng_v in engine_config.items():
        template = templates.get(eng_v)
        cmd = [eng_v]
        for required_key in template.get('required'):
            param = get_from_single_config(eng_k, required_key, True)
            if required_key.endswith('File') and required_key != 'inFile' and os.path.basename(param) == param:
                param = os.path.join(tar_dir, param)
            cmd.append('-{}'.format(required_key))
            cmd.append(param)

        for others_key in template.get('others'):
            param = get_from_single_config(eng_k, others_key, False)
            if param:
                cmd.append('-{}'.format(others_key))
                cmd.append(param)
        cmds.append(cmd)
    return cmds


def run_target(cmd):
    command = Popen(cmd, stdout=PIPE, stderr=PIPE, stdin=PIPE, shell=False)
    out = command.stdout.readlines()
    with open("log.txt", mode='a+', encoding='utf-8') as f:
        if len(out) > 0:
            f.writelines(' '.join(cmd) + "\r\n")
        for line in out:
            f.writelines(str(line.strip()) + "\r\n")

    success = True
    error = command.stderr.readlines()
    with open("error.txt", mode='a+', encoding='utf-8') as f:
        if len(error) > 0:
            f.writelines(' '.join(cmd) + "\r\n")

        for line in error:
            success = False
            f.writelines(str(line.strip()) + "\r\n")

    command.wait()
    return success


def run_with_engine(engine, jar):
    cmds = load_engine(engine)
    for cmd in cmds:
        cmd.insert(0, jar)
        cmd.insert(0, '-jar')
        cmd.insert(0, 'java')
        result = run_target(cmd)
        if not result:
            print("Command error on executing cmd, please check error.txt")
            print(' '.join(cmd))
            exit(1)
    print("Success!")
    pass


def do_sign(jar):
    sign_engine_config = {
        'sign.profile': 'sign-profile',
        'sign.app': 'sign-app'
    }
    run_with_engine(sign_engine_config, jar)


def do_sign_hap(jar):
    sign_hap_engine_config = {
        'sign.app': 'sign-app'
    }
    run_with_engine(sign_hap_engine_config, jar)


def do_sign_elf(jar):
    sign_elf_engine_config = {
        'sign.app': 'sign-app'
    }
    run_with_engine(sign_elf_engine_config, jar)


def do_generate(jar):
    cert_engine_config = {
        'app.keypair': 'generate-keypair',
        'profile.keypair': 'generate-keypair',
        'csr': 'generate-csr',
        'root-ca': 'generate-ca',
        'sub-ca.app': 'generate-ca',
        'sub-ca.profile': 'generate-ca',
        'cert.app': 'generate-app-cert',
        'cert.profile': 'generate-profile-cert',
    }
    run_with_engine(cert_engine_config, jar)


def do_generate_root_cert(jar):
    root_engine_config = {
        'profile.keypair': 'generate-keypair',
        'root-ca': 'generate-ca',
        'sub-ca.app': 'generate-ca',
        'sub-ca.profile': 'generate-ca',
        'cert.profile': 'generate-profile-cert',
    }
    run_with_engine(root_engine_config, jar)


def do_generate_app_cert(jar):
    app_cert_engine_config = {
        'app.keypair': 'generate-keypair',
        'cert.app': 'generate-app-cert',
    }
    run_with_engine(app_cert_engine_config, jar)


def do_sign_profile(jar):
    app_cert_engine_config = {
        'sign.profile': 'sign-profile',
    }
    run_with_engine(app_cert_engine_config, jar)


def convert_to_map(line, temp_map):
    line = line.strip('\n')
    strs = line.split('=', 1)

    if len(strs) == 2:
        if strs[1].startswith('$'):
            temp_map[strs[0]] = temp_map[strs[1][1:]]
        else:
            temp_map[strs[0]] = strs[1]


def load_config(config):
    config_file = config
    temp_map = {}
    with open(config_file, 'r', encoding='utf-8') as f:
        for line in f.readlines():
            if not re.match(r'\s*//[\s\S]*', line):
                convert_to_map(line, temp_map)

    for mk, mv in temp_map.items():
        strs = mk.rsplit('.', 1)
        if not global_config.get(strs[0]):
            global_config[strs[0]] = {}
        global_config[strs[0]][strs[-1]] = mv


def process_cmd():
    args = sys.argv
    if len(args) <= 1 or '--help' == args[1] or '-h' == args[1]:
        print_help()
        exit(0)

    action = args[1]
    if action not in ['createRootAndSubCert', 'createAppCertAndProfile', 'signHap', 'signElf']:
        print("Not support cmd")
        print_help()
        exit(1)
    return action


def process_jar():
    read_jar_file = global_config.get('config', {}).get('signtool')
    if not os.path.exists(read_jar_file):
        print("Jar file '{}' not found".format(read_jar_file))
        exit(1)
    return read_jar_file


def replace_cert_in_profile():
    profile_file = global_config.get('sign.profile', {}).get('inFile')
    app_cert_file = global_config.get('cert.app', {}).get('outFile')
    tar_dir = global_config.get('config', {}).get('targetDir')
    app_cert_file = os.path.join(tar_dir, app_cert_file)
    if not os.path.exists(profile_file):
        print("profile file '{}' not found".format(jar_file))
        exit(1)
    if not os.path.exists(app_cert_file):
        print("app cert file '{}' not found".format(jar_file))
        exit(1)

    app_cert = ''
    # read app cert
    with open(app_cert_file, 'r', encoding='utf-8') as f:
        app_cert_temp = f.read()
        app_cert = app_cert_temp.split("-----END CERTIFICATE-----")[0] + "-----END CERTIFICATE-----\n"

    profile = {}
    # read profile
    with open(profile_file, 'r', encoding='utf-8') as f:
        profile = json.load(f)

    try:
        profile["bundle-info"]["distribution-certificate"] = app_cert
    except KeyError:
        print("could not find distribution-certificate key in profile")

    # save profile
    flags = os.O_WRONLY | os.O_TRUNC
    modes = stat.S_IWUSR
    with os.fdopen(os.open(profile_file, flags, modes), 'w') as profile_write:
        json.dump(profile, profile_write)


if __name__ == '__main__':
    act = process_cmd()
    if act == 'createRootAndSubCert':
        load_config('createRootAndSubCert.config')
        jar_file = process_jar()
        do_generate_root_cert(jar_file)
    elif act == 'createAppCertAndProfile':
        load_config('createAppCertAndProfile.config')
        jar_file = process_jar()
        do_generate_app_cert(jar_file)
        replace_cert_in_profile()
        do_sign_profile(jar_file)
    elif act == 'signHap':
        load_config('signHap.config')
        jar_file = process_jar()
        do_sign_hap(jar_file)
    elif act == 'signElf':
        load_config('signElf.config')
        jar_file = process_jar()
        do_sign_elf(jar_file)
        