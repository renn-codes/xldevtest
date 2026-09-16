# wxl-runtime

Shared FrameScript and custom-packet services for WarcraftXL v1.1 extensions. The module keeps one
owner for the client packet hook and lets feature modules register handlers through published APIs.

## Services

- `wxl.framescript` v1 registers native Lua functions, scripts, and persistent client CVars.
- `wxl.network` v1 registers and sends WarcraftXL custom opcodes and supports non-owning server-message observers.

## Requirements

- WarcraftXL v1.1 with the FrameScript, network, and public-opcode core contracts.
- 32-bit World of Warcraft 3.3.5a client, build 12340.

This module has no client DB2/DBC payload and no server component by itself. Features consuming the
network service still require a server implementation with the same opcodes and packet layout.

## Installation

Install with WXL Hub. The release ZIP contains `wxl-runtime.dll`; the Hub places it under
`Extensions\\wxl-runtime`. Restart the client after installing or updating a native module.

For manual installation, extract the release ZIP to that same directory. Runtime services are always
enabled because dependent modules require them.

## Building

The release workflow builds this source as the `wxl-runtime` extension target in WarcraftXL v1.1.
Until the prerequisite core API change is merged upstream, build it against that review branch.
