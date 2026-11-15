import struct

for elf_path in ['gtalcs/gamefiles/android-libs/libopenal.so',
                 'gtalcs/gamefiles/android-libs/libVendor_mpg123.so',
                 'gtalcs/gamefiles/android-libs/libGame.so']:
    with open(elf_path, 'rb') as f:
        data = f.read()
    import re
    m = re.findall(rb'_ZNSt6__ndk16locale2id[a-zA-Z0-9_]*', data)
    print(f'{elf_path}: {set(x.decode() for x in m)}')
