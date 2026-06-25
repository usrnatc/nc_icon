"""
Enhanced Windows Folder Icon Setter & Artwork Renamer

This script performs two main functions recursively in a directory:
1. Renames common media artwork files (e.g., 'movie-poster.jpg') to
   standardized names (e.g., 'folder.jpg').
2. Creates high-quality, aspect-ratio-correct icons from poster images
   (e.g., folder.jpg) and configures the folders to use them.
"""

import os
import sys
import re
import argparse
from pathlib import Path
from PIL import Image, ImageFilter, ImageEnhance
import subprocess
import ctypes

# Check if running on Windows
if not sys.platform.startswith('win'):
    print("This script is designed to run on Windows only.")
    sys.exit(1)


class EnhancedFolderIconSetter:
    def __init__(self, dry_run=False, icon_size=256, rename=True, verbose=False):
        self.dry_run = dry_run
        self.icon_size = icon_size
        self.rename = rename
        self.verbose = verbose

    def _rename_artwork_files(self, root_path):
        """
        Recursively renames media artwork files to standardized names.
        Skips silently if the target filename already exists.
        """
        print("\n--- Starting Artwork Renaming Phase ---")
        rename_patterns = {
            r'^(.*)-poster\.([^.]+)$': r'folder.\2',
            r'^(.*)-clearlogo\.([^.]+)$': r'logo.\2',
            r'^(.*)-fanart\.([^.]+)$': r'fanart.\2',
            r'^(.*)-disc\.([^.]+)$': r'disc.\2',
            r'^(.*)-banner\.([^.]+)$': r'landscape.\2',
            r'^(.*)cover_art\.([^.]+)$': r'folder.\2',
        }
        renamed_count = 0

        # Use rglob for efficient recursive file searching
        for file_path in root_path.rglob('*'):
            if not file_path.is_file():
                continue

            filename = file_path.name
            for pattern, replacement in rename_patterns.items():
                if re.match(pattern, filename, re.IGNORECASE):
                    new_filename = re.sub(pattern, replacement, filename, flags=re.IGNORECASE)
                    new_file_path = file_path.with_name(new_filename)

                    # Silently skip if the target file already exists
                    if new_file_path.exists():
                        if self.verbose:
                            print(f"  - Skip rename (target exists): {new_file_path.name}")
                        break

                    if self.verbose or self.dry_run:
                        print(f"  {'[DRY RUN] Would rename' if self.dry_run else 'Renaming'}: {file_path.name} -> {new_filename}")

                    if not self.dry_run:
                        try:
                            file_path.rename(new_file_path)
                            renamed_count += 1
                        except OSError as e:
                            print(f"✗ Error renaming {file_path.name}: {e}")
                    break # Move to the next file once a pattern has matched

        summary = f"Renaming phase complete. {'Would have renamed' if self.dry_run else 'Renamed'} {renamed_count} files."
        print(f"✓ {summary}")
        return renamed_count

    def enhance_image_quality(self, image):
        if image.mode != 'RGBA':
            image = image.convert('RGBA')
        image = image.filter(ImageFilter.UnsharpMask(radius=1, percent=120, threshold=3))
        enhancer = ImageEnhance.Contrast(image)
        image = enhancer.enhance(1.1)
        return image

    def create_high_quality_icon(self, input_path, output_path):
        try:
            with Image.open(input_path) as img:
                img = self.enhance_image_quality(img)
                icon_sizes = [size for size in [256, 64, 48, 32, 24, 16] if size <= self.icon_size]
                if not icon_sizes: icon_sizes = [self.icon_size]

                icon_images = []
                for size in icon_sizes:
                    background = Image.new('RGBA', (size, size), (0, 0, 0, 0))
                    img_ratio = img.width / img.height
                    if img_ratio > 1.0:
                        new_width = size
                        new_height = int(new_width / img_ratio)
                    else:
                        new_height = size
                        new_width = int(new_height * img_ratio)
                    
                    if new_width < 1 or new_height < 1: continue
                    resized_poster = img.resize((new_width, new_height), Image.Resampling.LANCZOS)
                    paste_x = (size - new_width) // 2
                    paste_y = (size - new_height) // 2
                    background.paste(resized_poster, (paste_x, paste_y))
                    icon_images.append(background)

                if not icon_images:
                    print(f"✗ Could not generate any valid icon sizes for {input_path}")
                    return False

                if not self.dry_run:
                    icon_images[0].save(output_path, format='ICO', sizes=[(i.width, i.height) for i in icon_images], append_images=icon_images[1:])
            print(f"✓ Created aspect-ratio-correct icon: {output_path.name}")
            return True
        except Exception as e:
            print(f"✗ Error converting {input_path} to icon: {e}")
            return False

    def create_desktop_ini(self, folder_path, icon_file_name):
        desktop_ini = folder_path / "desktop.ini"
        content = f"[.ShellClassInfo]\nIconResource={icon_file_name},0\n"
        try:
            if not self.dry_run:
                with open(desktop_ini, 'w+', encoding='utf-8') as f:
                    f.write(content)
            print(f"✓ Created/updated: {desktop_ini.name}")
            return True
        except Exception as e:
            print(f"✗ Error creating {desktop_ini.name}: {e}")
            return False

    def set_folder_attributes(self, folder_path):
        try:
            if not self.dry_run:
                desktop_ini = folder_path / "desktop.ini"
                if desktop_ini.exists():
                    subprocess.run(['attrib', '+s', '+h', str(desktop_ini)], check=True, capture_output=True)
                subprocess.run(['attrib', '+r', str(folder_path)], check=True, capture_output=True)
            print(f"✓ Set attributes for folder")
            return True
        except subprocess.CalledProcessError as e:
            print(f"✗ Error setting attributes: {e.stderr.decode('utf-8').strip()}")
            return False

    def refresh_windows_explorer(self):
        try:
            if not self.dry_run:
                ctypes.windll.shell32.SHChangeNotify(0x08000000, 0, None, None)
            print("✓ Notified Windows Explorer to refresh icons.")
            return True
        except Exception as e:
            print(f"✗ Error refreshing Windows Explorer: {e}")
            return False

    def process_directory(self, root_dir):
        """
        Main processing function. Renames files then processes folders for icons.
        """
        root_path = Path(root_dir)
        if not root_path.is_dir():
            print(f"Error: Directory '{root_dir}' does not exist.")
            return False

        # --- 1. RENAME PHASE ---
        if self.rename:
            self._rename_artwork_files(root_path)

        # --- 2. ICON CREATION PHASE ---
        print("\n--- Starting Icon Creation Phase ---")
        poster_names = ['folder', 'poster']
        extensions = ['jpg', 'jpeg', 'png', 'webp', 'bmp']
        poster_images = []
        for name in poster_names:
            for ext in extensions:
                poster_images.extend(root_path.rglob(f'{name}.{ext}'))
                poster_images.extend(root_path.rglob(f'{name.capitalize()}.{ext}'))

        if not poster_images:
            print("No poster image files (e.g., folder.jpg) found to create icons from.")
            return False

        print(f"Found {len(poster_images)} potential poster images.")
        success_count = 0
        fail_count = 0
        processed_folders = set()

        for poster_image in poster_images:
            folder_path = poster_image.parent
            if folder_path in processed_folders: continue
            processed_folders.add(folder_path)

            # Silently skip if desktop.ini already exists
            if (folder_path / 'desktop.ini').exists():
                continue

            print(f"\nProcessing folder: {folder_path}")
            icon_file = folder_path / 'folder.ico'

            if self.create_high_quality_icon(poster_image, icon_file) and \
               self.create_desktop_ini(folder_path, icon_file.name) and \
               self.set_folder_attributes(folder_path):
                success_count += 1
            else:
                fail_count += 1

        if success_count > 0:
            self.refresh_windows_explorer()

        print(f"\n{'='*50}")
        print("Icon creation complete:")
        print(f"  Successfully configured folders: {success_count}")
        print(f"  Failed configurations: {fail_count}")
        print(f"  Total unique folders with posters: {len(processed_folders)}")

        if self.dry_run:
            print("\nNote: This was a dry run. No file system changes were made.")
        return success_count > 0


def main():
    parser = argparse.ArgumentParser(
        description="Rename media artwork and set folder icons from the resulting posters.",
        formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument('directory', nargs='?', default='.', help='Directory to search recursively (default: current directory)')
    parser.add_argument('--dry-run', '-d', action='store_true', help='Show what would be done without making any changes')
    parser.add_argument('--icon-size', '-s', type=int, default=256, choices=[16, 32, 48, 64, 128, 256], help='Maximum icon size (default: 256)')
    parser.add_argument('--no-rename', action='store_true', help='Skip the artwork renaming phase')
    parser.add_argument('--verbose', '-v', action='store_true', help='Show detailed output during the renaming phase')
    args = parser.parse_args()

    print("Enhanced Windows Folder Icon Setter & Renamer")
    print("=" * 50)
    print(f"Target directory: {Path(args.directory).resolve()}")
    if args.dry_run: print("Mode: Dry Run (no changes will be made)")
    if args.no_rename: print("Artwork renaming is DISABLED")
    print("-" * 50)

    setter = EnhancedFolderIconSetter(
        dry_run=args.dry_run,
        icon_size=args.icon_size,
        rename=not args.no_rename,
        verbose=args.verbose
    )
    setter.process_directory(args.directory)
    return 0


if __name__ == "__main__":
    sys.exit(main())
