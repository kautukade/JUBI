from __future__ import annotations

import os
from dataclasses import dataclass, fields, replace
from typing import Literal

# Default values for temperature and top_p
DEFAULT_TEMPERATURE = 0.7
DEFAULT_TOP_P = 1.0


def get_default_temperature() -> float:
    """Get the default temperature from environment or use DEFAULT_TEMPERATURE (0.7)."""
    try:
        return float(os.getenv("CAI_TEMPERATURE", str(DEFAULT_TEMPERATURE)))
    except (ValueError, TypeError):
        return DEFAULT_TEMPERATURE


def get_default_top_p() -> float:
    """Get the default top_p from environment or use DEFAULT_TOP_P (1.0)."""
    try:
        return float(os.getenv("CAI_TOP_P", str(DEFAULT_TOP_P)))
    except (ValueError, TypeError):
        return DEFAULT_TOP_P


@dataclass
class ModelSettings:
    """Settings to use when calling an LLM.

    This class holds optional model configuration parameters (e.g. temperature,
    top_p, penalties, truncation, etc.).

    Not all models/providers support all of these parameters, so please check the API documentation
    for the specific model and provider you are using.

    Default values:
    - temperature: 0.7 (can be overridden by CAI_TEMPERATURE env var)
    - top_p: 1.0 (can be overridden by CAI_TOP_P env var)
    """

    temperature: float | None = None
    """The temperature to use when calling the model. Default: 0.7 (from CAI_TEMPERATURE env var)."""

    top_p: float | None = None
    """The top_p to use when calling the model. Default: 1.0 (from CAI_TOP_P env var)."""

    frequency_penalty: float | None = None
    """The frequency penalty to use when calling the model."""

    presence_penalty: float | None = None
    """The presence penalty to use when calling the model."""

    tool_choice: Literal["auto", "required", "none"] | str | None = None
    """The tool choice to use when calling the model."""

    parallel_tool_calls: bool | None = None
    """Whether to use parallel tool calls when calling the model.
    Defaults to False if not provided."""

    truncation: Literal["auto", "disabled"] | None = None
    """The truncation strategy to use when calling the model."""

    max_tokens: int | None = None
    """The maximum number of output tokens to generate."""

    store: bool | None = None
    """Whether to store the generated model response for later retrieval.
    Defaults to True if not provided."""

    agent_model: str | None = None
    """The model from the Agent class. If set, this will override the model provided
    to the OpenAIChatCompletionsModel during initialization."""

    def resolve(self, override: ModelSettings | None) -> ModelSettings:
        """Produce a new ModelSettings by overlaying any non-None values from the
        override on top of this instance."""
        if override is None:
            return self

        changes = {
            field.name: getattr(override, field.name)
            for field in fields(self)
            if getattr(override, field.name) is not None
        }
        return replace(self, **changes)

    def with_defaults(self) -> ModelSettings:
        """Return a new ModelSettings with defaults applied for None values.

        Uses CAI_TEMPERATURE env var (default 0.7) and CAI_TOP_P env var (default 1.0).
        """
        return replace(
            self,
            temperature=self.temperature if self.temperature is not None else get_default_temperature(),
            top_p=self.top_p if self.top_p is not None else get_default_top_p(),
        )

    @classmethod
    def from_env(cls) -> ModelSettings:
        """Create ModelSettings with values from environment variables.

        Reads CAI_TEMPERATURE (default 0.7) and CAI_TOP_P (default 1.0).
        """
        return cls(
            temperature=get_default_temperature(),
            top_p=get_default_top_p(),
        )
