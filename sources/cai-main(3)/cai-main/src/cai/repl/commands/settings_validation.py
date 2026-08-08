"""API key and connectivity checks used by ``/settings validate`` and status views."""

import os
import asyncio
from typing import Dict, Optional, Tuple
from dataclasses import dataclass
from enum import Enum

# Try to import httpx for async HTTP, fall back to requests
try:
    import httpx
    HAS_HTTPX = True
except ImportError:
    HAS_HTTPX = False
    import requests


class ValidationStatus(Enum):
    """Status of a validation check."""
    VALID = "valid"
    INVALID = "invalid"
    ERROR = "error"
    NOT_SET = "not_set"
    SKIPPED = "skipped"


@dataclass
class ValidationResult:
    """Result of a validation check."""
    status: ValidationStatus
    message: str
    details: Optional[Dict] = None


# =============================================================================
# API Key Validators
# =============================================================================

def validate_openai_key(api_key: Optional[str] = None) -> ValidationResult:
    """Validate OpenAI API key by making a test request.

    Args:
        api_key: The API key to validate, or None to use env var

    Returns:
        ValidationResult with status and message
    """
    key = api_key or os.getenv("OPENAI_API_KEY")

    if not key:
        return ValidationResult(
            status=ValidationStatus.NOT_SET,
            message="OPENAI_API_KEY is not set"
        )

    if not key.startswith("sk-"):
        return ValidationResult(
            status=ValidationStatus.INVALID,
            message="OpenAI key should start with 'sk-'"
        )

    try:
        headers = {"Authorization": f"Bearer {key}"}
        if HAS_HTTPX:
            with httpx.Client(timeout=10.0) as client:
                response = client.get("https://api.openai.com/v1/models", headers=headers)
        else:
            response = requests.get(
                "https://api.openai.com/v1/models",
                headers=headers,
                timeout=10
            )

        if response.status_code == 200:
            return ValidationResult(
                status=ValidationStatus.VALID,
                message="OpenAI API key is valid",
                details={"models_available": True}
            )
        elif response.status_code == 401:
            return ValidationResult(
                status=ValidationStatus.INVALID,
                message="OpenAI API key is invalid or expired"
            )
        elif response.status_code == 429:
            return ValidationResult(
                status=ValidationStatus.VALID,
                message="OpenAI API key is valid (rate limited)",
                details={"rate_limited": True}
            )
        else:
            return ValidationResult(
                status=ValidationStatus.ERROR,
                message=f"Unexpected response: {response.status_code}"
            )
    except Exception as e:
        return ValidationResult(
            status=ValidationStatus.ERROR,
            message=f"Connection error: {str(e)}"
        )


def validate_anthropic_key(api_key: Optional[str] = None) -> ValidationResult:
    """Validate Anthropic API key.

    Args:
        api_key: The API key to validate, or None to use env var

    Returns:
        ValidationResult with status and message
    """
    key = api_key or os.getenv("ANTHROPIC_API_KEY")

    if not key:
        return ValidationResult(
            status=ValidationStatus.NOT_SET,
            message="ANTHROPIC_API_KEY is not set"
        )

    if not key.startswith("sk-ant-"):
        return ValidationResult(
            status=ValidationStatus.INVALID,
            message="Anthropic key should start with 'sk-ant-'"
        )

    try:
        headers = {
            "x-api-key": key,
            "anthropic-version": "2023-06-01",
            "content-type": "application/json"
        }
        # Use a minimal request to check the key
        if HAS_HTTPX:
            with httpx.Client(timeout=10.0) as client:
                response = client.post(
                    "https://api.anthropic.com/v1/messages",
                    headers=headers,
                    json={
                        "model": "claude-3-haiku-20240307",
                        "max_tokens": 1,
                        "messages": [{"role": "user", "content": "hi"}]
                    }
                )
        else:
            response = requests.post(
                "https://api.anthropic.com/v1/messages",
                headers=headers,
                json={
                    "model": "claude-3-haiku-20240307",
                    "max_tokens": 1,
                    "messages": [{"role": "user", "content": "hi"}]
                },
                timeout=10
            )

        if response.status_code in [200, 201]:
            return ValidationResult(
                status=ValidationStatus.VALID,
                message="Anthropic API key is valid"
            )
        elif response.status_code == 401:
            return ValidationResult(
                status=ValidationStatus.INVALID,
                message="Anthropic API key is invalid"
            )
        elif response.status_code == 429:
            return ValidationResult(
                status=ValidationStatus.VALID,
                message="Anthropic API key is valid (rate limited)"
            )
        else:
            return ValidationResult(
                status=ValidationStatus.ERROR,
                message=f"Unexpected response: {response.status_code}"
            )
    except Exception as e:
        return ValidationResult(
            status=ValidationStatus.ERROR,
            message=f"Connection error: {str(e)}"
        )


def validate_openrouter_key(api_key: Optional[str] = None) -> ValidationResult:
    """Validate OpenRouter API key.

    Args:
        api_key: The API key to validate, or None to use env var

    Returns:
        ValidationResult with status and message
    """
    key = api_key or os.getenv("OPENROUTER_API_KEY")

    if not key:
        return ValidationResult(
            status=ValidationStatus.NOT_SET,
            message="OPENROUTER_API_KEY is not set"
        )

    try:
        headers = {"Authorization": f"Bearer {key}"}
        if HAS_HTTPX:
            with httpx.Client(timeout=10.0) as client:
                response = client.get(
                    "https://openrouter.ai/api/v1/models",
                    headers=headers
                )
        else:
            response = requests.get(
                "https://openrouter.ai/api/v1/models",
                headers=headers,
                timeout=10
            )

        if response.status_code == 200:
            return ValidationResult(
                status=ValidationStatus.VALID,
                message="OpenRouter API key is valid"
            )
        elif response.status_code == 401:
            return ValidationResult(
                status=ValidationStatus.INVALID,
                message="OpenRouter API key is invalid"
            )
        else:
            return ValidationResult(
                status=ValidationStatus.ERROR,
                message=f"Unexpected response: {response.status_code}"
            )
    except Exception as e:
        return ValidationResult(
            status=ValidationStatus.ERROR,
            message=f"Connection error: {str(e)}"
        )


def validate_alias_key(api_key: Optional[str] = None) -> ValidationResult:
    """Validate Alias Robotics API key (OpenAI-compatible listing endpoint)."""
    key = (api_key or os.getenv("ALIAS_API_KEY") or "").strip()
    if not key:
        return ValidationResult(
            status=ValidationStatus.NOT_SET,
            message="ALIAS_API_KEY is not set",
        )
    url = "https://api.aliasrobotics.com:666/v1/models"
    try:
        headers = {"Authorization": f"Bearer {key}"}
        if HAS_HTTPX:
            with httpx.Client(timeout=10.0) as client:
                response = client.get(url, headers=headers)
        else:
            response = requests.get(url, headers=headers, timeout=10)
        if response.status_code == 200:
            return ValidationResult(
                status=ValidationStatus.VALID,
                message="Alias API key is valid",
            )
        if response.status_code in (401, 403):
            return ValidationResult(
                status=ValidationStatus.INVALID,
                message="Alias API key is invalid or not authorized",
            )
        return ValidationResult(
            status=ValidationStatus.ERROR,
            message=f"Unexpected response: {response.status_code}",
        )
    except Exception as e:
        return ValidationResult(
            status=ValidationStatus.ERROR,
            message=f"Connection error: {str(e)}",
        )


def validate_google_key(api_key: Optional[str] = None) -> ValidationResult:
    """Validate Google API key.

    Args:
        api_key: The API key to validate, or None to use env var

    Returns:
        ValidationResult with status and message
    """
    key = api_key or os.getenv("GOOGLE_API_KEY")

    if not key:
        return ValidationResult(
            status=ValidationStatus.NOT_SET,
            message="GOOGLE_API_KEY is not set"
        )

    try:
        # Test with Gemini API
        url = f"https://generativelanguage.googleapis.com/v1/models?key={key}"
        if HAS_HTTPX:
            with httpx.Client(timeout=10.0) as client:
                response = client.get(url)
        else:
            response = requests.get(url, timeout=10)

        if response.status_code == 200:
            return ValidationResult(
                status=ValidationStatus.VALID,
                message="Google API key is valid"
            )
        elif response.status_code in [401, 403]:
            return ValidationResult(
                status=ValidationStatus.INVALID,
                message="Google API key is invalid or lacks permissions"
            )
        else:
            return ValidationResult(
                status=ValidationStatus.ERROR,
                message=f"Unexpected response: {response.status_code}"
            )
    except Exception as e:
        return ValidationResult(
            status=ValidationStatus.ERROR,
            message=f"Connection error: {str(e)}"
        )


# =============================================================================
# Ollama / Local Model Validators
# =============================================================================

def check_ollama_running(base_url: Optional[str] = None) -> ValidationResult:
    """Check if Ollama server is running and accessible.

    Args:
        base_url: Ollama API base URL, or None to auto-detect

    Returns:
        ValidationResult with status and message
    """
    # Determine the base URL to check
    url = base_url or os.getenv("OLLAMA_API_BASE") or "http://127.0.0.1:11434"

    # Remove /v1 suffix if present for the root check
    if url.endswith("/v1"):
        url = url[:-3]

    try:
        if HAS_HTTPX:
            with httpx.Client(timeout=5.0) as client:
                response = client.get(url)
        else:
            response = requests.get(url, timeout=5)

        if response.status_code == 200:
            text = response.text.lower()
            if "ollama" in text:
                return ValidationResult(
                    status=ValidationStatus.VALID,
                    message=f"Ollama is running at {url}",
                    details={"url": url}
                )
            else:
                return ValidationResult(
                    status=ValidationStatus.VALID,
                    message=f"Server responded at {url} (may not be Ollama)"
                )
        else:
            return ValidationResult(
                status=ValidationStatus.ERROR,
                message=f"Server returned status {response.status_code}"
            )
    except Exception as e:
        return ValidationResult(
            status=ValidationStatus.INVALID,
            message=f"Cannot connect to Ollama at {url}: {str(e)}",
            details={
                "url": url,
                # Keys resolved via ``tr()`` in the settings UI (any language)
                "suggestion_keys": [
                    "ollama_sugg_serve",
                    "ollama_sugg_firewall",
                    "ollama_sugg_api_base",
                ],
            },
        )


def list_ollama_models(base_url: Optional[str] = None) -> Tuple[bool, list]:
    """List available models in Ollama.

    Args:
        base_url: Ollama API base URL

    Returns:
        Tuple of (success, list of model names)
    """
    url = base_url or os.getenv("OLLAMA_API_BASE") or "http://127.0.0.1:11434"

    # Remove /v1 suffix for tags endpoint
    if url.endswith("/v1"):
        url = url[:-3]

    try:
        if HAS_HTTPX:
            with httpx.Client(timeout=10.0) as client:
                response = client.get(f"{url}/api/tags")
        else:
            response = requests.get(f"{url}/api/tags", timeout=10)

        if response.status_code == 200:
            data = response.json()
            models = [m.get("name", "") for m in data.get("models", [])]
            return True, models
        return False, []
    except Exception:
        return False, []


# =============================================================================
# Network Connectivity Checks
# =============================================================================

def check_network_connectivity() -> ValidationResult:
    """Check general network connectivity to LLM providers.

    Returns:
        ValidationResult with connectivity status
    """
    endpoints = [
        ("OpenAI", "https://api.openai.com"),
        ("Anthropic", "https://api.anthropic.com"),
        ("OpenRouter", "https://openrouter.ai"),
        ("Google", "https://generativelanguage.googleapis.com"),
    ]

    results = {}
    any_success = False

    for name, url in endpoints:
        try:
            if HAS_HTTPX:
                with httpx.Client(timeout=5.0) as client:
                    response = client.head(url)
            else:
                response = requests.head(url, timeout=5)
            results[name] = response.status_code < 500
            if response.status_code < 500:
                any_success = True
        except Exception:
            results[name] = False

    if any_success:
        return ValidationResult(
            status=ValidationStatus.VALID,
            message="Network connectivity OK",
            details=results
        )
    else:
        return ValidationResult(
            status=ValidationStatus.INVALID,
            message="Cannot reach any LLM provider",
            details=results
        )


# =============================================================================
# Comprehensive Validation
# =============================================================================

def validate_all_api_keys() -> Dict[str, ValidationResult]:
    """Validate all configured API keys.

    Returns:
        Dictionary mapping key names to validation results
    """
    validators = {
        "ALIAS_API_KEY": validate_alias_key,
        "OPENAI_API_KEY": validate_openai_key,
        "ANTHROPIC_API_KEY": validate_anthropic_key,
        "OPENROUTER_API_KEY": validate_openrouter_key,
        "GOOGLE_API_KEY": validate_google_key,
    }

    results = {}
    for key_name, validator in validators.items():
        results[key_name] = validator()

    return results


def get_configuration_status() -> Dict[str, any]:
    """Get comprehensive configuration status.

    Returns:
        Dictionary with all configuration checks
    """
    status = {
        "api_keys": validate_all_api_keys(),
        "ollama": check_ollama_running(),
        "network": check_network_connectivity(),
    }

    # Check for Ollama models if Ollama is running
    if status["ollama"].status == ValidationStatus.VALID:
        success, models = list_ollama_models()
        status["ollama_models"] = models if success else []

    return status
