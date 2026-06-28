# Copyright (c) 2014-present PlatformIO <contact@platformio.org>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import platform
import shutil
import subprocess
import sys
from site import addsitedir

IS_WINDOWS = sys.platform.startswith("win")
INITIAL_FILES = list(os.listdir())
PLATFORM_NAME = platform.system().lower()
CONTRIB_DIR = os.path.join(os.getcwd(), "_contrib")

esptool_deps = [
    "cryptography ~= 45.0.3",
    "ecdsa ~= 0.19.1",
    "bitstring ~= 4.3.1",
    "reedsolo ~= 1.7.0",
    "intelhex ~= 2.3.0"
]

def install_pypi_pkgs(deps, skip_prebuilt=False):
    args = [
        os.path.normpath(sys.executable),
        "-m",
        "pip",
        "install",
        "--no-compile",
        "-t",
        CONTRIB_DIR,
    ]
    if skip_prebuilt:
        args.extend(["--no-binary", ":all:"])
    try:
        subprocess.run(args + deps, check=True, env=os.environ)
    except subprocess.CalledProcessError as exc:
        if "linux" in PLATFORM_NAME:
            raise Exception(
                "Please ensure that the following packages are installed:\n\n"
                "sudo apt install python3-dev libffi-dev libssl-dev\n"
            ) from exc
        raise exc


def clean():
    print("Cleaning...")
    for name in os.listdir():
        if name in (".", ".."):
            continue
        if name not in INITIAL_FILES:
            if os.path.isfile(name):
                os.remove(name)
            else:
                shutil.rmtree(name)


def main():
    try:
        if not os.path.isdir(CONTRIB_DIR):
            os.makedirs(CONTRIB_DIR)
        install_pypi_pkgs(
            esptool_deps,
            skip_prebuilt=False,
        )
    except Exception as exc:
        print("\n\n%s\n" % exc)
        clean()
        # make package unusable
        for name in ("package.json", ".piopm"):
            if os.path.isfile(name):
                os.remove(name)


if __name__ == "__main__":
    main()
