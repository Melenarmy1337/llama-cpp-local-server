# llama.cpp Local Server for Windows

A portable Windows launcher and C++ proxy for running GGUF models with
[llama.cpp](https://github.com/ggml-org/llama.cpp). It exposes both the
OpenAI-compatible API and the Ollama API without requiring Ollama itself.

The project is intended for a CUDA-capable NVIDIA GPU, but it discovers the
installed llama.cpp backend at runtime. No model, binary, log, local path, or
credential is part of this repository.

## What it provides

- A single `start.bat` entry point; no PowerShell scripts are required.
- Native C++ process manager and Ollama/OpenAI proxy.
- OpenAI-compatible API via llama.cpp on port `8080`.
- Ollama-compatible API and OpenAI forwarding on port `11434`.
- Model selection via the API `model` field or `start.bat switch <fragment>`.
- Automatic context fallback if the requested RAM/VRAM allocation cannot load.
- CUDA auto-offload, Flash Attention, KV-cache offload, RAM loading, and
  performance-oriented defaults.
- Detailed request diagnostics without storing full prompts or responses.
- Optional firewall rules restricted to the Windows Private profile and
  `LocalSubnet`.

## Repository layout

```text
config/models.preset.ini  llama.cpp router preset generated at startup
src/lamacpp_local.cpp     C++ manager and Ollama/OpenAI proxy
start.bat                 Windows entry point
bin/                      local llama.cpp and build output, ignored by Git
models/                   local GGUF models, ignored by Git
logs/                     generated diagnostics, ignored by Git
state/                    generated process state, ignored by Git
downloads/                temporary downloads, ignored by Git
```

## Requirements

1. Windows 10 or Windows 11.
2. A current NVIDIA driver for CUDA GPU acceleration.
3. Current [llama.cpp Windows CUDA binaries](https://github.com/ggml-org/llama.cpp/releases).
4. A GGUF model compatible with your llama.cpp version.
5. Visual Studio Build Tools or Visual Studio with the C++ desktop workload,
   only when compiling the manager from source.

## Setup

1. Create the directories below next to this README if they do not exist:

   ```bat
   mkdir bin models logs state downloads
   ```

2. Download and unpack the llama.cpp Windows CUDA release into `bin`. The
   directory must contain `bin\llama-server.exe` and the DLLs shipped with the
   same release, including the CUDA DLLs.

3. Put one or more `.gguf` files directly into `models`.

4. Build the manager in an x64 Visual Studio Developer Command Prompt:

   ```bat
   cl /nologo /std:c++20 /EHsc /O2 /W4 /D_WIN32_WINNT=0x0A00 /DWIN32_LEAN_AND_MEAN src\lamacpp_local.cpp /Fo:bin\lamacpp_local.obj /Fe:bin\lamacpp-local.exe /link ws2_32.lib shell32.lib advapi32.lib
   ```

5. Start it from the repository root:

   ```bat
   start.bat
   ```

The foreground mode runs diagnostics, starts llama.cpp and the C++ proxy,
performs API smoke tests, then keeps the server alive. Stop it with `Ctrl+C`.

## Commands

```bat
start.bat                 Run in the foreground, including self-tests
start.bat start           Start llama.cpp and the proxy in the background
start.bat stop            Stop both background processes
start.bat status          Show process, model, firewall, and API status
start.bat switch Qwen     Select the first GGUF whose name contains Qwen
start.bat bench           Run a small direct OpenAI API benchmark
start.bat downloads       List local files in downloads
start.bat firewall        Add Private/LocalSubnet firewall rules using UAC
```

`switch` writes the selected model name to `state/default-model.txt` and
preloads it when the server is running. API clients can also send a model alias
in the usual `model` property.

## APIs

The llama.cpp server is directly available as an OpenAI-compatible endpoint:

```text
http://127.0.0.1:8080/v1
GET  /v1/models
POST /v1/chat/completions
```

The proxy offers the Ollama endpoints:

```text
http://127.0.0.1:11434/api
GET  /api/version
GET  /api/tags
GET  /api/ps
POST /api/show
POST /api/chat
POST /api/generate
```

It also forwards OpenAI-compatible calls on:

```text
http://127.0.0.1:11434/v1
```

Example OpenAI request:

```json
{
  "model": "your-model-alias",
  "messages": [{"role": "user", "content": "Say hello."}],
  "max_tokens": 64
}
```

Example Ollama request:

```json
{
  "model": "your-model-alias",
  "prompt": "Say hello.",
  "options": {"num_predict": 64},
  "stream": false
}
```

For an Ollama-style request without `num_predict` or `max_tokens`, the proxy
uses an `8192`-token output limit. Set an explicit limit in client requests
when a different cap is required.

## Performance profile

The defaults target single-chat throughput on a 12 GB NVIDIA GPU and use:

- `--no-mmap` to load model data into RAM.
- `--n-gpu-layers auto`, `--fit on`, and `--fit-target 512` for GPU offload.
- `--flash-attn auto` and `--kv-offload on`.
- `q8_0` key/value cache.
- Context fallback from `262144` down to `8192` tokens.
- `threads = 8`, `threads-batch = 8`, `batch-size = 2048`, and
  `ubatch-size = 512`.

The best values depend on model size, quantization, GPU VRAM, system RAM, CUDA
driver, and concurrent requests. Use `start.bat bench` after changing
`config/models.preset.ini`, and leave enough VRAM headroom for stable loading.

## Logging and privacy

The proxy writes `logs/proxy-detailed.log` and
`logs/proxy-requests.jsonl`. Entries include metadata such as route, selected
model, request size, token limits, timing, finish reason, and token counts.
Only a short request preview is recorded; full prompts and generated responses
are not deliberately logged.

Treat logs as private operational data. They are excluded from version control
by `.gitignore`.

## LAN access

The server listens on all local interfaces. To allow access from devices on the
same private network, run:

```bat
start.bat firewall
```

The command requests elevation and creates inbound rules only for the Windows
Private profile and `LocalSubnet` on TCP ports `8080` and `11434`. Do not expose
these unauthenticated endpoints directly to the internet.

## Security and publishing

This repository intentionally excludes:

- GGUF model weights and model directories.
- llama.cpp binaries, CUDA DLLs, build objects, and download archives.
- Runtime logs, state, local configuration overrides, and environment files.
- Private keys, certificates, and common secret file formats.

Before publishing a fork, inspect `git status --ignored` and never add a model,
log, `.env`, API key, token, or user-specific configuration. Use environment
variables or a secrets manager for integrations that require authentication.

## Verification

Run the foreground launcher with a model present. It checks CUDA devices,
llama.cpp health, OpenAI `/v1/models` and chat completions, and the Ollama
`/api/version`, `/api/tags`, `/api/chat`, and `/api/generate` endpoints. A
successful run ends with `READY context=<value> tests=OK`.

## Upstream references

- [llama.cpp server documentation](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md)
- [llama.cpp releases](https://github.com/ggml-org/llama.cpp/releases)
- [Ollama API](https://docs.ollama.com/api/introduction)
- [Ollama OpenAI compatibility](https://docs.ollama.com/api/openai-compatibility)
