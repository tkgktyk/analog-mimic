import os
import zipfile

def create_library_zip(source_dir, registers_header_src, zip_output_path):
    """
    Creates a ZIP archive for the Arduino library, excluding hidden files/folders,
    and injects the shared mimic_registers.h into the src directory.
    """
    with zipfile.ZipFile(zip_output_path, 'w', zipfile.ZIP_DEFLATED) as zipf:
        # 1. Walk through the source directory
        for root, dirs, files in os.walk(source_dir):
            
            # Exclude hidden directories (e.g., .pio, .vscode) from recursive walk
            dirs[:] = [d for d in dirs if not d.startswith('.')]
            
            for file in files:
                # Exclude hidden files
                if file.startswith('.'):
                    continue
                
                # Get full path and relative path (setting the root as "Mimic")
                full_path = os.path.join(root, file)
                rel_path = os.path.relpath(full_path, os.path.dirname(source_dir))
                
                zipf.write(full_path, rel_path)
                print(f"Added: {rel_path}")

        # 2. Inject the external mimic_registers.h directly into Mimic/src/
        registers_zip_path = os.path.join("Mimic", "src", "mimic_registers.h")
        zipf.write(registers_header_src, registers_zip_path)
        print(f"Added: {registers_zip_path} (Recovered from core)")

def main():
    # 1. Path definitions
    # Calculate relative paths based on the script's execution location
    base_dir = os.path.dirname(os.path.abspath(__file__))
    
    # Source paths
    software_mimic_dir = os.path.join(base_dir, "Mimic")
    registers_h_source = os.path.join(base_dir, "..", "firmware", "core", "Inc", "mimic_registers.h")
    
    # Output destination (directly under the software directory)
    zip_output_path = os.path.join(base_dir, "Mimic.zip")

    # Basic validation (Guard clauses)
    if not os.path.exists(software_mimic_dir):
        print(f"Error: Directory '{software_mimic_dir}' not found. Please check the script execution path.")
        return
        
    if not os.path.exists(registers_h_source):
        print(f"Error: File '{registers_h_source}' not found.")
        return

    print("Starting ZIP compression for the Arduino library...")

    # 2. Execute ZIP creation
    create_library_zip(software_mimic_dir, registers_h_source, zip_output_path)

    print("-" * 40)
    print(f"Success: ZIP file created -> {zip_output_path}")

if __name__ == "__main__":
    main()
