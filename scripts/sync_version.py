import re

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

def main():
    ini_path = "platformio.ini"
    lib_path = "library.properties"
    version = get_version_from_ini(ini_path)
    if not version:
        print("VERSION not found in platformio.ini")
        return
    update_library_properties(lib_path, version)

if __name__ == "__main__":
    main()

def before_build(*args, **kwargs): main()