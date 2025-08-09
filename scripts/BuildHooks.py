import os
import re
import shutil

def get_src_dir():
    # Read src_dir from platformio.ini
    ini_path = "platformio.ini"
    src_dir = None
    with open(ini_path, "r") as f:
        for line in f:
            m = re.match(r"\s*src_dir\s*=\s*([^\s]+)", line)
            if m:
                src_dir = m.group(1)
                break
    return src_dir

def get_version_from_ini(ini_path):
    with open(ini_path, "r") as f:
        for line in f:
            m = re.match(r"\s*-D\s+VERSION\s*=\s*([^\s\\]+)", line)
            if m:
                return m.group(1)
    return None

def update_library_properties(lib_path, new_version):
    lines = []
    updated = False
    with open(lib_path, "r") as f:
        for line in f:
            if line.startswith("version="):
                current_version = line.strip().split("=", 1)[1]
                if current_version != new_version:
                    print(f"Updating library.properties version: {current_version} -> {new_version}")
                    line = f"version={new_version}\n"
                    updated = True
            lines.append(line)
    if updated:
        with open(lib_path, "w") as f:
            f.writelines(lines)
    else:
        print("library.properties version already matches.")

def rename_ino_to_cpp():
    src_dir = get_src_dir()
    if not src_dir:
        print("No src_dir set in platformio.ini")
        return
    folder = os.path.basename(src_dir)
    ino_path = os.path.join(src_dir, f"{folder}.ino")
    cpp_path = os.path.join(src_dir, f"{folder}.cpp")
    if os.path.exists(ino_path):
        print(f"Pre Build Renaming {ino_path} -> {cpp_path}")
        shutil.move(ino_path, cpp_path)

def rename_cpp_to_ino():
    src_dir = get_src_dir()
    if not src_dir:
        print("No src_dir set in platformio.ini")
        return
    folder = os.path.basename(src_dir)
    cpp_path = os.path.join(src_dir, f"{folder}.cpp")
    ino_path = os.path.join(src_dir, f"{folder}.ino")
    if os.path.exists(cpp_path):
        print(f"Post Build Renaming {cpp_path} -> {ino_path}")
        shutil.move(cpp_path, ino_path)

def sync_version():
    ini_path = "platformio.ini"
    lib_path = "library.properties"
    version = get_version_from_ini(ini_path)
    if not version:
        print("VERSION not found in platformio.ini")
    else:
        update_library_properties(lib_path, version)

def before_build(*args, **kwargs):
    sync_version()
    rename_ino_to_cpp()

def after_build(*args, **kwargs):
    rename_cpp_to_ino()

def before_upload(*args, **kwargs):
    pass

def after_upload(*args, **kwargs):
    pass

def before_test(*args, **kwargs):
    pass

def after_test(*args, **kwargs):
    pass

def before_clean(*args, **kwargs):
    pass

def after_clean(*args, **kwargs):
    pass