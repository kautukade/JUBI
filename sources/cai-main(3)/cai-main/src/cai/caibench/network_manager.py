import docker
import ipaddress

class NetworkManager:
    def __init__(self, network_name, subnet):
        self.network_name = network_name
        self.subnet = subnet
        self.client = docker.from_env()

    def create_network(self):
        ipam_config = docker.types.IPAMConfig(
            pool_configs=[
                docker.types.IPAMPool(subnet=self.subnet)
            ]
        )
        network = self.client.networks.create(
            self.network_name,
            driver='bridge',
            ipam=ipam_config
        )
        return network

    def get_network(self):
        try:
            return self.client.networks.get(self.network_name)
        except docker.errors.NotFound:
            return None

    def remove_network(self):
        try:
            network = self.client.networks.get(self.network_name)
            network.remove()
            print(f"Network '{self.network_name}' removed successfully.")
        except docker.errors.NotFound:
            print(f"Network '{self.network_name}' not found.")
        except docker.errors.APIError as e:
            print(f"Failed to remove network '{self.network_name}': {e}")

    def get_available_ip(self):
        """
        Get an available IP address from the subnet.
        """
        network = ipaddress.IPv4Network(self.subnet)
        for ip in network.hosts():
            # Skip gateway IPs (usually .1, .254)
            if ip.packed[-1] in [1, 254]:
                continue
            return str(ip)
        return None