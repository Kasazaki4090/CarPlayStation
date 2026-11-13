import os

def main():
    png_path = os.path.join('UI', 'Win32', 'resources', 'oem_icon.png')
    with open(png_path, 'rb') as f:
        data = f.read()

    header_path = os.path.join('Core', 'Session', 'OemIconAsset.h')
    with open(header_path, 'w', encoding='utf-8') as f:
        f.write('#pragma once\n')
        f.write('#include <cstdint>\n')
        f.write('#include <cstddef>\n\n')
        f.write('namespace CarPlayStation::Core::Session {\n\n')
        f.write('extern const uint8_t kDefaultOemIconPng[];\n')
        f.write('extern const size_t kDefaultOemIconPngSize;\n\n')
        f.write('} // namespace CarPlayStation::Core::Session\n')

    cpp_path = os.path.join('Core', 'Session', 'OemIconAsset.cpp')
    with open(cpp_path, 'w', encoding='utf-8') as f:
        f.write('#include "OemIconAsset.h"\n\n')
        f.write('namespace CarPlayStation::Core::Session {\n\n')
        f.write(f'const size_t kDefaultOemIconPngSize = {len(data)};\n\n')
        f.write('const uint8_t kDefaultOemIconPng[] = {\n')
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            hex_str = ', '.join(f'0x{b:02x}' for b in chunk)
            f.write(f'    {hex_str},\n')
        f.write('};\n\n')
        f.write('} // namespace CarPlayStation::Core::Session\n')

    print(f'Successfully generated {header_path} and {cpp_path} ({len(data)} bytes)')

if __name__ == '__main__':
    main()

