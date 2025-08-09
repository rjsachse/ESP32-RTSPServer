import os
import shutil

Import("env")

def get_src_dir():
    config = env.GetProjectConfig()
    return config.get("platformio", "src_dir")

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

rename_ino_to_cpp()