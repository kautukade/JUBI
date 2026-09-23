# VPS test plans

Jubi's autonomous developer does not receive an unrestricted shell.

If a project needs executable tests, an administrator may add a JSON plan here.
The model cannot write this directory when Jubi runs under the hardened VPS
service. Example:

    {"argv":["python3","-m","unittest","discover","-v"],"timeout":120}

Allowed executable families are Python, Node and npm. Static Python/JSON/JS
verification does not require a test plan.
