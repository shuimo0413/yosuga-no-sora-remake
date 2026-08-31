#!/usr/bin/env python3
"""Sign HAPs with the pinned OpenHarmony community test certificate chain.

Why pinned: hvigor-side community signing that regenerates a fresh root CA
on every build produces HAPs whose root is not in the trust store of real
OpenHarmony devices (KaihongOS 5.0 et al.), so they fail to install with
"verify signature failed" (code 9568329). The pinned chain is generated
once from the official OpenHarmony test keystore (ohtest.p12, public
materials, password 123456) and stays stable across builds, so every
published HAP carries a root that devices accept.

For every input hap this script:
1. writes a release provisioning profile bound to --bundle-name with the
   pinned app certificate as its distribution-certificate,
2. signs the profile with the pinned profile certificate chain,
3. signs the hap, writing <hap-basename>-signed.hap into --out-dir.
"""

import argparse
import json
import os
import subprocess
import sys
import time
import uuid


def read_leaf_cert(pem_path: str) -> str:
    text = open(pem_path, encoding="utf-8").read()
    parts = text.split("-----END CERTIFICATE-----")
    if not parts or "-----BEGIN CERTIFICATE-----" not in parts[0]:
        raise SystemExit("error: no certificate found in %s" % pem_path)
    return parts[0] + "-----END CERTIFICATE-----\n"


def run_java(java: str, jar: str, args: list) -> None:
    cmd = [java, "-jar", jar] + args
    proc = subprocess.run(cmd, capture_output=True, text=True)
    out = (proc.stdout or "") + (proc.stderr or "")
    for line in out.strip().splitlines()[-3:]:
        print("   ", line)
    if proc.returncode != 0:
        raise SystemExit("error: command failed: %s" % " ".join(cmd))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jar", required=True, help="hap-sign-tool.jar path")
    parser.add_argument("--java", default="java", help="java executable")
    parser.add_argument("--keystore", required=True, help="pinned ohtest.p12")
    parser.add_argument("--keystore-pwd", default="123456")
    parser.add_argument("--app-cert", required=True, help="pinned app cert chain (pem)")
    parser.add_argument("--key-alias", default="oh-app1-key-v1")
    parser.add_argument("--key-pwd", default="123456")
    parser.add_argument("--profile-cert", required=True, help="pinned profile cert chain (pem)")
    parser.add_argument("--profile-key-alias", default="oh-profile-key-v1")
    parser.add_argument("--bundle-name", required=True)
    parser.add_argument("--validity-years", type=int, default=10)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("haps", nargs="+", help="unsigned HAPs to sign")
    args = parser.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    work = os.path.join(args.out_dir, "profile-work")
    os.makedirs(work, exist_ok=True)

    leaf = read_leaf_cert(args.app_cert)
    now = int(time.time())
    profile = {
        "version-name": "2.0.0",
        "version-code": 2,
        "app-distribution-type": "os_integration",
        "uuid": str(uuid.uuid4()),
        "validity": {
            "not-before": now - 86400,
            "not-after": now + args.validity_years * 365 * 86400,
        },
        "type": "release",
        "bundle-info": {
            "developer-id": "OpenHarmony",
            "distribution-certificate": leaf,
            "bundle-name": args.bundle_name,
            "apl": "normal",
            "app-feature": "hos_normal_app",
        },
        # Strict ROMs (KaihongOS) only grant system_basic-level permissions
        # when the signing profile lists them in allowed-acls, and
        # restricted ones additionally in restricted-permissions. Without
        # these the app's sockets are denied (every http.request() hangs,
        # download stays at 0 bytes forever) and the download-directory
        # permission dialog can never succeed - while other devices on the
        # same network work fine because their grant policy auto-approves.
        "acls": {"allowed-acls": [
            "ohos.permission.INTERNET",
            "ohos.permission.READ_WRITE_DOWNLOAD_DIRECTORY",
        ]},
        "permissions": {"restricted-permissions": [
            "ohos.permission.READ_WRITE_DOWNLOAD_DIRECTORY",
        ]},
        "issuer": "pki_internal",
    }
    profile_json = os.path.join(work, "profile.json")
    with open(profile_json, "w", encoding="utf-8") as fh:
        json.dump(profile, fh, indent=2)

    profile_p7b = os.path.join(work, "profile.p7b")
    run_java(args.java, args.jar, [
        "sign-profile", "-mode", "localSign",
        "-keyAlias", args.profile_key_alias, "-keyPwd", args.key_pwd,
        "-profileCertFile", args.profile_cert,
        "-inFile", profile_json,
        "-signAlg", "SHA256withECDSA",
        "-keystoreFile", args.keystore, "-keystorePwd", args.keystore_pwd,
        "-outFile", profile_p7b,
    ])
    print("profile signed:", profile_p7b)

    for hap in args.haps:
        base = os.path.splitext(os.path.basename(hap))[0]
        signed = os.path.join(args.out_dir, base + "-signed.hap")
        run_java(args.java, args.jar, [
            "sign-app", "-mode", "localSign",
            "-keyAlias", args.key_alias, "-keyPwd", args.key_pwd,
            "-appCertFile", args.app_cert,
            "-profileFile", profile_p7b,
            "-inFile", hap,
            "-signAlg", "SHA256withECDSA",
            "-keystoreFile", args.keystore, "-keystorePwd", args.keystore_pwd,
            "-outFile", signed,
        ])
        print("signed:", signed)

    missing = [h for h in args.haps
               if not os.path.exists(os.path.join(
                   args.out_dir,
                   os.path.splitext(os.path.basename(h))[0] + "-signed.hap"))]
    if missing:
        raise SystemExit("error: signing produced no output for %s" % missing)
    return 0


if __name__ == "__main__":
    sys.exit(main())
