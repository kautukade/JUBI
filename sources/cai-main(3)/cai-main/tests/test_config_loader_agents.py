from cai.config_loader import extract_agent_definitions


def test_extract_agent_definitions_parallel_agents():
    data = {
        "description": "Example",
        "shared": {"prompt": "Shared task", "auto_run": False},
        "parallel_agents": [
            {"name": "alpha_agent", "team": "Team A", "env": {"FLAG": True}},
            {
                "name": "beta_agent",
                "prompt": "Custom",
                "auto_run": True,
                "model": "alias1",
            },
        ],
    }

    agents, metadata, origin = extract_agent_definitions(data)

    assert origin == "parallel_agents"
    assert metadata["shared_prompt"] == "Shared task"
    assert metadata["auto_run"] is False
    assert len(agents) == 2

    first, second = agents
    assert first["agent_name"] == "alpha_agent"
    assert first["team"] == "Team A"
    assert first["prompt"] == "Shared task"
    assert first["env"] == {"FLAG": True}
    assert first["auto_run"] is False

    assert second["agent_name"] == "beta_agent"
    assert second["prompt"] == "Custom"
    assert second["model"] == "alias1"
    assert second["auto_run"] is True


def test_extract_agent_definitions_tui_startup_legacy():
    data = {
        "tui_startup": {
            "shared_prompt": "Legacy shared",
            "auto_run": False,
            "teams": [
                {
                    "name": "Legacy Team",
                    "prompt": "Team prompt",
                    "agents": [
                        {"name": "alpha_agent", "auto_run": True},
                        {"name": "beta_agent"},
                    ],
                }
            ],
        }
    }

    agents, metadata, origin = extract_agent_definitions(data)

    assert origin == "tui_startup"
    assert metadata["shared_prompt"] == "Legacy shared"
    assert metadata["auto_run"] is False
    assert len(agents) == 2

    alpha, beta = agents
    assert alpha["team"] == "Legacy Team"
    assert alpha["prompt"] == "Team prompt"
    assert alpha["auto_run"] is True
    assert beta["prompt"] == "Team prompt"
    assert beta["auto_run"] is False
