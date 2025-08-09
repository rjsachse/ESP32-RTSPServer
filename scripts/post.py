import os
import shutil

Import("env")

def get_src_dir():
    config = env.GetProjectConfig()
    return config.get("platformio", "src_dir")

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

rename_cpp_to_ino()