"""Host-side GGUF memory policy for the iiLocalLLM daemon.

The caller owns the returned process and controls its lifetime. These settings
do not allocate memory until the daemon loads a model.
"""
from dataclasses import dataclass
from os import PathLike, fspath
from subprocess import Popen
from typing import Sequence


@dataclass(frozen=True)
class LlamaMemoryOptions:
    kv_cache: str = "f16"
    gpu_layers: int = -1

    def __post_init__(self):
        if self.kv_cache not in ("f16", "q8_0", "q4_0"):
            raise ValueError("kv_cache must be f16, q8_0, or q4_0")
        if type(self.gpu_layers) is not int or not -1 <= self.gpu_layers <= 1048576:
            raise ValueError("gpu_layers must be -1 or an integer in [0, 1048576]")

    def daemon_arguments(self) -> list[str]:
        return ["--llama-kv-cache", self.kv_cache, f"--llama-gpu-layers={self.gpu_layers}"]


def start_daemon(executable: str | PathLike, options: LlamaMemoryOptions,
                 arguments: Sequence[str] = (), **popen_options) -> Popen:
    """Start a daemon using explicit host options; the caller must stop/wait it."""
    if not isinstance(options, LlamaMemoryOptions):
        raise TypeError("options must be LlamaMemoryOptions")
    if isinstance(arguments, (str, bytes)):
        raise TypeError("arguments must be a sequence of arguments, not one string")
    return Popen([fspath(executable), *arguments, *options.daemon_arguments()], **popen_options)
