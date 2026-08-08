import json
from pathlib import Path
import time
from .ctf import CTF, update_readme_with_benchmark_tables



def ctf(name=None, list=None, **kwargs):
    """
    Initialize a CTF object and report execution time.

    Takes about 0.0057 seconds to find a CTF within the existing 1500 CTFs.
    """
    start_time = time.time()  # Start timing

    if list or name:
        package_dir = Path(__file__).parent
        config_file = package_dir / 'ctf-jsons' / 'ctf_configs.jsonl'

        with config_file.open('r') as file:
            configs = json.load(file)
            if name:
                for config in configs:
                    if config['name'].lower() == name.lower():
                        if 'container_name' in config:
                            config['container_name'] = config['container_name'].lower()
                        # Update the config with any provided kwargs
                        config = {**config, **kwargs}
                        end_time = time.time()  # End timing
                        print(f"CAIBench, time it took to find CTF: {end_time - start_time:.4f} seconds")
                        return CTF(config)
                raise ValueError(f"No CTF configuration found for '{name}'")

    if not list and not name:
        raise ValueError("Either 'name' or 'list' must be provided")

    end_time = time.time()  # End timing
    print(f"CAIBench, time it took to find CTF: {end_time - start_time:.4f} seconds")


def list():
    """
    List all CTFs.
    """
    package_dir = Path(__file__).parent
    config_file = package_dir / 'ctf-jsons' / 'ctf_configs.jsonl'

    ctf_list = []
    with config_file.open('r') as file:
        configs = json.load(file)
        for config in configs:
            ctf_info = {
                'name': config.get('name'),
                'difficulty': config.get('difficulty'), 
                'type': config.get('type'),
                'challenge': config.get('challenge')
            }
            ctf_list.append(ctf_info)
    
    return ctf_list