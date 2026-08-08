from abc import ABC, abstractmethod
import docker
from .network_manager import NetworkManager
import os
import json
from pprint import pprint
from wasabi import color


class CTFSetupError(Exception):
    """Exception raised when CTF setup fails."""
    pass


class CTF(ABC):
    def __init__(self, config):
        self.name = config['name']
        self.subnet = config.get('subnet', '192.168.3.0/24')
        self.ip_address = config.get('ip_address')
        # Add support for instance ID to make container names unique
        instance_id = os.environ.get('CTF_INSTANCE_ID', '')
        base_container_name = config.get('container_name', "ctf_target").lower()
        self.container_name = f"{base_container_name}{instance_id}" if instance_id else base_container_name

        # Handle IP address assignment for parallel execution
        if instance_id:
            # Extract base IP from subnet (e.g., 192.168.3 from 192.168.3.0/24)
            subnet_parts = self.subnet.split('/')
            base_ip = '.'.join(subnet_parts[0].split('.')[:-1])

            # Parse instance number from instance_id (e.g., "_1" -> 1)
            instance_num = int(instance_id.replace('_', '')) if instance_id.replace('_', '').isdigit() else hash(instance_id) % 240 + 1

            # Calculate offset: instance 1 gets offset 0, instance 2 gets offset 1, etc.
            offset = instance_num - 1

            # Reserved IPs that should not be assigned to CTF targets
            # 192.168.3.5 is reserved for the attacker/agent
            RESERVED_IPS = [5]

            # If there's a hardcoded IP, use it as base and add offset
            if self.ip_address:
                # Extract the last octet and add instance offset
                original_last_octet = int(self.ip_address.split('.')[-1])
                # For parallel instances: base IP + offset
                # Instance 1 gets original IP, instance 2 gets original+1, etc.
                new_last_octet = original_last_octet + offset
                if new_last_octet > 254:
                    new_last_octet = 10 + (new_last_octet % 240)  # Wrap around

                # Check if the calculated IP conflicts with reserved IPs
                if new_last_octet in RESERVED_IPS:
                    print(color(f"WARNING: IP 192.168.3.{new_last_octet} is reserved (attacker IP). Skipping to next available IP.", fg="yellow", bold=True))
                    new_last_octet += 1
                    if new_last_octet > 254:
                        new_last_octet = 10

                self.ip_address = f"{base_ip}.{new_last_octet}"
            else:
                # No hardcoded IP, use default starting IP of .100
                # This range (100+) is safe from the reserved attacker IP (.5)
                default_base_octet = 100
                new_last_octet = default_base_octet + offset
                if new_last_octet > 254:
                    new_last_octet = 10 + (new_last_octet % 240)  # Wrap around

                # Additional safety check (shouldn't be needed with base=100, but defensive)
                if new_last_octet in RESERVED_IPS:
                    print(color(f"WARNING: IP 192.168.3.{new_last_octet} is reserved (attacker IP). Adjusting to safe range.", fg="yellow", bold=True))
                    new_last_octet = 100

                self.ip_address = f"{base_ip}.{new_last_octet}"
        else:
            # When no instance_id is set, use default IP if not already configured
            if not self.ip_address:
                # Extract base IP from subnet (e.g., 192.168.3 from 192.168.3.0/24)
                subnet_parts = self.subnet.split('/')
                base_ip = '.'.join(subnet_parts[0].split('.')[:-1])
                self.ip_address = f"{base_ip}.100"

        self.image = config['image']
        self.command = config.get('command')
        self.init_command = config.get('init_command')
        self.entrypoint = config.get('entrypoint')  
        self.port_bindings = config.get('port_bindings', {})
        self.mac_address = config.get('mac_address')
        self.challenges = config.get('challenges', {})
        self.flag_commands = config.get('flag_commands', {})
        self.flags = {}
        self.instructions = config.get('instructions', '')
        self.type = config.get('type', 'IT')
        self.difficulty = config.get('difficulty', 'Medium')
        self.techniques = config.get('techniques', '')
        self.description = config.get('description', '')
        self.latent = config.get('latent', "true")
        # If OS X, we need to use the right docker socket
        #  see https://github.com/docker/docker-py/issues/3059
        if os.name == 'posix' and os.uname().sysname == 'Darwin':
            os.environ["DOCKER_HOST"] = f"unix://{os.path.expanduser('~')}/.docker/run/docker.sock"
            self.client = docker.from_env()
        else:
            self.client = docker.from_env()

        self.container = None
        self._remove_existing_container()
        self.network_name = "CAIBench"
        self.network_manager = self._create_or_get_network()
        self.registry_url = "registry.gitlab.com"
        self.registry_username = "gitlab"
        self.registry_password = os.environ.get('CAIBENCH_IMG_REGISTRY_TOKEN', '')


        print(color(f"Initializing CTF: {self.name}", fg="green", bold=True))
        print(color(f"Description: ", fg="white", bold=True), color(self.description, fg="white"))
        print(color(f"Type: {self.type}", fg="blue"))
        print(color(f"Difficulty: {self.difficulty}", fg="yellow"))
        print(color(f"Techniques: {self.techniques}", fg="pink"))
        print(color(f"Instructions: {self.instructions}", fg="cyan"))
        if self.init_command:
            print(color(f"Init Command: {self.init_command}", fg="magenta"))
        if self.entrypoint:
            print(color(f"Entrypoint: {self.entrypoint}", fg="magenta"))
        #
        print(color("Challenges:", fg="cyan"))
        pprint(self.challenges)
        #
        print(color("Flag commands:", fg="cyan"))
        pprint(self.flag_commands)

    def get_challenges(self):
        try:
            if not self.challenges:
                return "No challenges defined for this CTF"

            results = []
            for challenge_name, command in self.challenges.items():
                if not self.container:
                    print("Container is not running. Attempting to retrieve the container.")
                    self._find_container()
                    if not self.container:
                        print(f"Container {self.container_name} not found. Cannot execute challenge '{challenge_name}'.")
                        continue

                try:
                    result = self.container.exec_run(command)
                    output = result.output.decode('utf-8').strip()
                    results.append(f"Challenge: {challenge_name}\nOutput: {output}")
                except docker.errors.APIError as e:
                    results.append(f"Failed to execute challenge '{challenge_name}': {str(e)}")

            return "\n\n".join(results)
        except Exception as e:
            return f"An error occurred while retrieving challenges: {str(e)}"

    def _create_or_get_network(self):
        existing_networks = self.client.networks.list()
        for network in existing_networks:
            ipam_configs = network.attrs.get('IPAM', {}).get('Config', []) or []
            for config in ipam_configs:

                # # debug
                # print("---")
                # print(f"Network Name: {network.name}")
                # print(f"Config: {config}")
                # print(f"Subnet: {config.get('Subnet')}")
                # print(f"Self subnet: {self.subnet}")
                if config.get('Subnet') == self.subnet:
                    # # debug
                    # print(f"Using existing network '{network.name}' with subnet '{self.subnet}'.")
                    self.network_name = network.name
                    return NetworkManager(network.name, self.subnet)
        try:
            subnet_parts = self.subnet.split('.')
            self.network_name = f"{self.network_name}_{'_'.join(subnet_parts[:3])}_{subnet_parts[3].split('/')[0]}"
            network = self.client.networks.create(
                name=self.network_name,
                driver="bridge",
                ipam=docker.types.IPAMConfig(
                    pool_configs=[docker.types.IPAMPool(subnet=self.subnet)]
                )
            )
            # # debug
            # print(f"Created new network '{self.network_name}' with subnet '{self.subnet}'.")
            return NetworkManager(self.network_name, self.subnet)
        except docker.errors.APIError as e:
            print(f"Failed to create network '{self.network_name}': {e}")
            raise

    def _remove_existing_container(self):
        try:
            existing_container = self.client.containers.get(self.container_name)
            existing_container.stop()
            existing_container.remove()
            print(f"Removed existing container: {self.container_name}")
        except docker.errors.NotFound:
            pass

    def cleanup(self):
        try:
            self.stop_ctf()
            if self.network_manager:
                self.network_manager.remove_network()
            if self.client:
                self.client.close()
        except Exception as e:
            print(f"Error during cleanup: {e}")

    def _execute_init_command(self):
        """Execute the init_command outside the Docker container on the host machine."""
        if not self.init_command:
            return
            
        try:
            print(f"Executing init command on host machine: {self.init_command}")
            import subprocess
            import os
            
            # Execute the command on the host machine
            result = subprocess.run(
                self.init_command,
                shell=True,
                capture_output=True,
                text=True,
                cwd=os.getcwd()
            )
            
            if result.returncode == 0:
                print(f"Init command executed successfully: {result.stdout.strip()}")
            else:
                print(f"Init command failed with error: {result.stderr.strip()}")
                
        except Exception as e:
            print(f"Error executing init command: {e}")

    def start_ctf(self):
        # Execute init_command first (outside Docker container)
        self._execute_init_command()
        
        max_retries = 3
        for attempt in range(max_retries):
            try:
                # Authenticate and pull the image
                self._authenticate_and_pull_image()

                network_config = self.network_manager.client.api.create_networking_config({
                    self.network_manager.network_name: self.network_manager.client.api.create_endpoint_config(
                        ipv4_address=self.ip_address
                    ) if self.ip_address else self.network_manager.client.api.create_endpoint_config()
                })

                # # debug
                # print(f"Starting CTF with network name: {self.network_name}")

                host_config = self.network_manager.client.api.create_host_config(
                    port_bindings=self.port_bindings,
                    privileged=True,
                    network_mode=self.network_name,
                    publish_all_ports=True
                )
    
                container_config = {
                    'image': self.image,
                    'name': self.container_name,
                    'hostname': self.name,
                    'host_config': host_config,
                    'networking_config': network_config,
                    'tty': True,
                    'stdin_open': True,
                    'detach': True
                }
                
                # Add entrypoint if specified
                if self.entrypoint:
                    container_config['entrypoint'] = self.entrypoint
                    print(f"Setting entrypoint to: {self.entrypoint}")
                    # When using a custom entrypoint, we need to pass the command as arguments
                    container_config['command'] = ["-c", "tail -f /dev/null"]
                else:
                    # Default command when no custom entrypoint
                    container_config['command'] = "tail -f /dev/null"
                
                # Add mac_address if specified
                if self.mac_address:
                    container_config['mac_address'] = self.mac_address
                
                container = self.network_manager.client.api.create_container(**container_config)

                self.network_manager.client.api.start(container=container.get('Id'))
                try:
                    self.container = self.client.containers.get(container.get('Id'))
                except docker.errors.NotFound:
                    print(f"Container ID {container.get('Id')} not found. Attempting to locate by name '{self.container_name}'.")
                    self._find_container()

                if not self.container:
                    if attempt < max_retries - 1:
                        print(f"Failed to locate container, retrying... (Attempt {attempt + 1}/{max_retries})")
                        continue
                    else:
                        print("Max retries reached. Failed to start container.")
                        return
                    
                self._get_flags()  # populate self.flags

                # Start command if defined
                if self.command and self.latent.lower() == "true":
                    print(f"Starting command: {self.command}")
                    self.get_shell(self.command, detach=True)
                
                elif self.command:
                    print(f"Executing initial command: {self.command}")
                    try:
                        # Check if command should run in background (ends with &)
                        if self.command.rstrip().endswith('&'):
                            print("Command ends with &, running in detached mode")
                            output = self.get_shell(self.command, detach=True)
                        else:
                            output = self.get_shell(self.command)
                        if output:
                            print(f"Command output: {output}")
                        else:
                            print("Command executed but no output returned")
                    except Exception as e:
                        print(f"Warning: Initial command failed: {e}")
                        print("Container started but initial setup may be incomplete")
                # debug    
                print(color("Flags:", fg="cyan"))
                pprint(self.flags)
                print(color(f"Started CTF: {self.name}", fg="green"))
                print(color(f"IP Address: {self.get_ip()}", fg="green"))
                break

            except docker.errors.APIError as e:
                print(f"Failed to start {self.name} container: {e}")
                if attempt < max_retries - 1:
                    print(f"Retrying... (Attempt {attempt + 1}/{max_retries})")
                else:
                    print("Max retries reached. Failed to start container.")
                    
    def _authenticate_and_pull_image(self):
        # First check if image exists locally
        try:
            self.client.images.get(self.image)
            print(color(f"Image {self.image} found locally", fg="green"))
            return
        except docker.errors.ImageNotFound:
            pass

        if not self.registry_username or not self.registry_password:
            raise CTFSetupError("GitLab registry credentials not found in environment variables.")

        max_retries = 3
        for attempt in range(max_retries):
            try:
                self.client.login(
                    username=self.registry_username,
                    password=self.registry_password,
                    registry=self.registry_url
                )
                self.client.images.pull(self.image)
                break  # Exit the loop if successful
            except docker.errors.APIError as e:
                print(color(f"Attempt {attempt + 1} - Failed to authenticate or pull image: {e}", fg="yellow"))
                if attempt < max_retries - 1:
                    print(color("Retrying...", fg="yellow"))
                else:
                    raise CTFSetupError(f"Max retries reached. Failed to authenticate or pull image: {e}")
            except Exception as e:
                raise CTFSetupError(f"An unexpected error occurred while pulling image: {e}")
    def stop_ctf(self):
        try:
            if self.container:
                self.container.stop()
                self.container.remove()
                print(f"{self.name} CTF stopped and removed successfully.")
        except docker.errors.NotFound:
            print(f"{self.name} CTF container not found.")
        except docker.errors.APIError as e:
            print(f"Failed to stop/remove {self.name} container: {e}")

    def get_flag(self, challenge=None):
        """Get the flag for the given challenge. If none is provided,
        the flag for the first challenge is returned.

        Args:
            challenge (str, optional): The challenge to get the flag for. 
            Defaults to None.

        Returns:
            str: The flag for the given challenge.
        """
        try:
            if challenge is None:
                return self.flags[list(self.challenges.keys())[0]]
            return self.flags[challenge]
        except (KeyError, IndexError):
            if not self.flags:
                return "!!!!CONTAINER NOT RUNNING"
            return list(self.flags.values())[0]
    
        # NOTE: contributed by @luijait, but this disrupts assumption
        # that all CTFs must have the challenges defined in the JSON
        #
        # try:
        #     if challenge is None:
        #         return self.flags[list(self.challenges.keys())[0]]
        #     return self.flags[challenge]
        # except (KeyError, IndexError):
        #     if not self.flags:
        #         return "No flags available"
        #     return list(self.flags.values())[0]

    def _get_flags(self):
        try:
            for label, cmd in self.flag_commands.items():
                if not self.container:
                    print("Container is not running. Attempting to retrieve the container.")
                    self._find_container()
                    if not self.container:
                        print(f"Container {self.container_name} not found. Cannot retrieve flag for '{label}'.")
                        continue
                result = self.container.exec_run(cmd)
                try:
                    flag = result.output.decode('utf-8').strip()
                    if isinstance(flag, bytes):
                        flag = flag.decode('utf-8')
                except UnicodeDecodeError:
                    flag = result.output[:100].strip()
                self.flags[label] = flag
        except Exception as e:
            print(f"An error occurred while retrieving the flags: {str(e)}")
            # Don't return here, just log the error and continue

    def get_instructions(self):
        return self.instructions
    
    def get_techniques(self):
        return self.techniques    

    def get_challenges(self):
        return self.challenges

    def check_flag(self, output, challenge):
        """Check if the output contains the flag for the given challenge.

        Args:
            output (str): The output from the container.
            challenge (str): The challenge to check the flag for.

        Returns:
            bool: True if the flag is found in the output, False otherwise.
        """
        if not isinstance(output, str):
            return False
        if challenge not in self.flags:
            print(f"Flag for challenge '{challenge}' not found.")
            return False
        flag = self.flags[challenge]

        # NOTE: this is not a perfect solution, but it works for now
        if isinstance(flag, bytes):
            flag = repr(flag)[2:-1]  # remove the quotes, b' and '

        # # debug
        # print(f"Flag: {flag}")
        # print(f"Output: {output}")

        return flag in output

    def get_ip(self):
        return self.ip_address

    def get_shell(self, command, detach=False, timeout=60):
        # print(f"get_shell called with command: {command}")
        if self.container:
            # print(f"Container {self.container_name} is running. Attempting to execute command.")
            try:
                # Use a shell to execute the command to handle multiple commands properly
                if detach:
                    # For background commands, we need to ensure the process stays alive
                    # Remove trailing & if present since we'll handle backgrounding
                    cmd = command.rstrip().rstrip('&').strip()
                    # Use exec to replace the shell process, avoiding issues with process termination
                    # Start the command directly in detached mode without additional shell wrappers
                    exec_cmd = ["/bin/sh", "-c", f"exec {cmd}"]
                    result = self.container.exec_run(exec_cmd, detach=True)
                    return "Command started in background"
                else:
                    result = self.container.exec_run(f"/bin/sh -c 'timeout {timeout} {command}'", 
                                                   tty=True, stdin=True, stdout=True, stderr=True)
                    # print(f"Command executed. Decoding result.")
                    try:
                        return_result = result.output.decode('utf-8').strip()
                    except UnicodeDecodeError:
                        # print("Failed to decode output as UTF-8, returning raw bytes representation.")
                        return_result = repr(result.output)
                    # print(f"Return result: {return_result}")
                    return return_result

            except docker.errors.NotFound:
                print(f"Exec instance not found for command '{command}'. Attempting to locate the container.")
                self._find_container()
                if self.container:
                    print(f"Container {self.container_name} found after re-locating. Retrying command execution.")
                    try:
                        if detach:
                            # For background commands, we need to ensure the process stays alive
                            cmd = command.rstrip().rstrip('&').strip()
                            exec_cmd = ["/bin/sh", "-c", f"exec {cmd}"]
                            result = self.container.exec_run(exec_cmd, detach=True)
                            return "Command started in background"
                        else:
                            result = self.container.exec_run(f"/bin/sh -c '{command}'",
                                                           tty=True, stdin=True, stdout=True, stderr=True)
                            print(f"Command executed after re-locating. Decoding result.")
                            try:
                                return_result = result.output.decode('utf-8').strip()
                            except UnicodeDecodeError:
                                print("Failed to decode output as UTF-8, returning raw bytes representation.")
                                return_result = repr(result.output)
                            return return_result
                    except docker.errors.APIError as e:
                        print(f"Failed to execute command in {self.name} container after re-locating: {e}")
                        return None
                else:
                    print(f"Container {self.container_name} could not be found after re-locating.")
                    return None
            except docker.errors.APIError as e:
                print(f"Failed to execute command in {self.name} container: {e}")
                return None
        else:
            print(f"Container for {self.name} is not running.")
            return None

    def _find_container(self):
        try:
            containers = self.client.containers.list()
            for container in containers:
                if container.name == self.container_name:
                    self.container = container
                    print(f"Container '{self.container_name}' found with ID {container.id}.")
                    return
            print(f"Container '{self.container_name}' not found among running containers.")
            self.container = None
        except docker.errors.APIError as e:
            print(f"Error while searching for container '{self.container_name}': {e}")
            self.container = None

def generate_benchmark_tables_by_caibench():
    """
    Returns a dict mapping caibench values to markdown tables for each benchmark.
    caibench values: 'cybersec', 'base', 'rctf2', 'cyber_range'
    """
    # Map caibench values to display names and README section markers
    caibench_map = {
        "cybench": {
            "display": '"Cybench" Benchmark',
            "readme_section": '"Cybench" Benchmark'
        },
        "base": {
            "display": '"Base" Benchmark',
            "readme_section": '"Base" Benchmark'
        },
        "rctf2": {
            "display": '"RCTF2" Benchmark',
            "readme_section": '"RCTF2" Benchmark'
        },
        "cyber_range": {
            "display": '"Cyber Ranges" Benchmark',
            "readme_section": '"Cyber Ranges" Benchmark'
        },
        "auto-pen-bench": {
            "display": '"Auto-Pen-Bench" Benchmark',
            "readme_section": '"Auto-Pen-Bench" Benchmark'
        }
    }

    # Table header
    def table_header():
        return "| # | Name | Difficulty | # Challenges | Challenge/Technique | Source | Container |\n" \
               "|---|------|------------|--------------------|--------------------|--------|-----------|\n"

    # Load CTFs from JSON file
    ctf_jsons_folder = os.path.join(os.path.dirname(__file__), 'ctf-jsons')
    caibench_tables = {k: "" for k in caibench_map}
    ctf_names = set()
    try:
        with open(os.path.join(ctf_jsons_folder, 'ctf_configs.jsonl'), 'r') as f:
            ctf_configs = json.load(f)
            # Group CTFs by caibench value
            caibench_ctfs = {k: [] for k in caibench_map}
            for ctf in ctf_configs:
                name = ctf.get('name')
                if not name or name in ctf_names:
                    continue
                # Only include if works is exactly the string "true"
                if str(ctf.get("works", "")).lower() != "true":
                    continue
                caibench_val = ctf.get('caibench', '').strip().lower()
                if caibench_val in caibench_map:
                    caibench_ctfs[caibench_val].append(ctf)
                ctf_names.add(name)
            # Sort each list by difficulty
            difficulty_order = {'very easy': 1, 'easy': 2, 'medium': 3, 'hard': 4, 'very hard': 5}
            for caibench_val, ctf_list in caibench_ctfs.items():
                ctf_list.sort(key=lambda x: difficulty_order.get(str(x.get('difficulty', 'Medium')).lower(), 3))
                if ctf_list:
                    md = table_header()
                    counter = 1
                    for ctf in ctf_list:
                        try:
                            name = ctf.get('name', 'N/A').lower().replace(' ', '-')
                            difficulty = ctf.get('difficulty', 'N/A')
                            # Count number of challenges/flags
                            num_challenges = 0
                            if 'challenges' in ctf and isinstance(ctf['challenges'], dict):
                                num_challenges = len(ctf['challenges'])
                            elif 'flag_commands' in ctf and isinstance(ctf['flag_commands'], dict):
                                num_challenges = len(ctf['flag_commands'])
                            else:
                                num_challenges = ""
                            challenge_or_technique = ctf.get('challenge', ctf.get('techniques', 'N/A'))
                            source = ctf.get('source', 'Unknown')
                            container = ctf.get('image', 'N/A')
                            md += f"| {counter} | `{name}` | {difficulty} | {num_challenges} | {challenge_or_technique} | {source} | {container} |\n"
                            counter += 1
                        except KeyError as e:
                            print(f"An unexpected error occurred: {str(e)}")
                    caibench_tables[caibench_val] = md
    except (json.JSONDecodeError, FileNotFoundError) as e:
        print(f"Error loading CTF configs: {str(e)}")
    return caibench_tables

def update_readme_with_benchmark_tables():
    """
    For each caibench value, finds the corresponding <details> section in README.md and
    inserts the generated table for that benchmark.
    """
    caibench_tables = generate_benchmark_tables_by_caibench()
    # Map caibench values to section markers in README.md
    section_titles = {
        "cybench": '"Cybench" Benchmark',
        "base": '"Base" Benchmark',
        "rctf2": '"RCTF2" Benchmark',
        "cyber_range": '"Cyber Ranges" Benchmark',
        "auto-pen-bench": '"Auto-Pen-Bench" Benchmark'
    }
    try:
        with open(os.path.join(os.path.dirname(__file__), 'README.md'), 'r') as file:
            content = file.read()

        new_content = content
        for caibench_val, table_md in caibench_tables.items():
            section_title = section_titles[caibench_val]
            
            # Find the <details> section for this benchmark
            details_start = new_content.find(f'<details>\n<summary>{section_title}</summary>')
            if details_start == -1:
                details_start = new_content.find(f'<details>\n<summary>{section_title}</summary>\n')
            if details_start == -1:
                print(f"Could not find section for {section_title} in README.md.")
                continue
            details_end = new_content.find("</details>", details_start)
            if details_end == -1:
                print(f"Could not find end of <details> for {section_title} in README.md.")
                continue
            # Replace the content between <summary>...</summary> and </details>
            summary_end = new_content.find("</summary>", details_start)
            if summary_end == -1:
                print(f"Could not find </summary> for {section_title} in README.md.")
                continue
            summary_end += len("</summary>")
            # Everything between summary_end and details_end is the old content
            before = new_content[:summary_end]
            after = new_content[details_end:]
            # Insert two newlines, then the table, then two newlines
            new_details = before + "\n\n" + table_md + "\n" + after
            new_content = new_details

        with open(os.path.join(os.path.dirname(__file__), 'README.md'), 'w') as file:
            file.write(new_content)
        print("README.md updated successfully with the new benchmark tables.")
    except FileNotFoundError:
        print("README.md not found")
    except IOError as e:
        print(f"An error occurred while reading or writing the file: {str(e)}")
    except Exception as e:
        print(f"An unexpected error occurred: {str(e)}")

def update_main_benchmarks_readme():
    """
    Updates the main benchmarks/README.md file with the generated benchmark tables.
    This function targets the main project README instead of the caibench-specific one.
    """
    caibench_tables = generate_benchmark_tables_by_caibench()
    # Map caibench values to section markers in README.md
    section_titles = {
        "cybench": '"Cybench" Benchmark',
        "base": '"Base" Benchmark',
        "rctf2": '"RCTF2" Benchmark',
        "cyber_range": '"Cyber Ranges" Benchmark',
        "auto-pen-bench": '"Auto-Pen-Bench" Benchmark'
    }
    try:
        # Use the main benchmarks README.md file
        readme_path = os.path.join(os.path.dirname(__file__), '..', '..', '..', 'benchmarks', 'README.md')
        with open(readme_path, 'r') as file:
            content = file.read()

        new_content = content
        for caibench_val, table_md in caibench_tables.items():
            section_title = section_titles[caibench_val]
            
            # Find the <details> section for this benchmark
            details_start = new_content.find(f'<details>\n<summary>{section_title}</summary>')
            if details_start == -1:
                details_start = new_content.find(f'<details>\n<summary>{section_title}</summary>\n')
            if details_start == -1:
                # If section not found, append a new one at the end
                append_block = f"\n\n<details>\n<summary>{section_title}</summary>\n\n{table_md}\n\n</details>\n"
                new_content = new_content + append_block
                continue
            details_end = new_content.find("</details>", details_start)
            if details_end == -1:
                print(f"Could not find end of <details> for {section_title} in benchmarks/README.md.")
                continue
            # Replace the content between <summary>...</summary> and </details>
            summary_end = new_content.find("</summary>", details_start)
            if summary_end == -1:
                print(f"Could not find </summary> for {section_title} in benchmarks/README.md.")
                continue
            summary_end += len("</summary>")
            # Everything between summary_end and details_end is the old content
            before = new_content[:summary_end]
            after = new_content[details_end:]
            # Insert two newlines, then the table, then two newlines
            new_details = before + "\n\n" + table_md + "\n" + after
            new_content = new_details

        with open(readme_path, 'w') as file:
            file.write(new_content)
        print("benchmarks/README.md updated successfully with the new benchmark tables.")
    except FileNotFoundError:
        print("benchmarks/README.md not found")
    except IOError as e:
        print(f"An error occurred while reading or writing the file: {str(e)}")
    except Exception as e:
        print(f"An unexpected error occurred: {str(e)}")
