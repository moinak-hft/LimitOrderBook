# Source attribution

This project extends a seven-file C++ limit order book snapshot supplied by the project owner after discovering it on Reddit. The supplied folder did not include Git history, a repository URL, or a license file.

The original source headers credit **Oscar Zhang**:

- `original/Order.h`: created 2026/2/22
- `original/OrderBook.h`: created 2026/2/23
- `original/Trade.h`: created 2026/2/28

The seven supplied files were preserved in [`original/`](../original/) before the matching engine and replay lab were extended. Their original comments and README remain available there. The current engine, benchmark suite, simulator, interactive report, tests, and build helpers were developed from that starting point.

No upstream repository URL or license is asserted here. This attribution document does not add a license to the supplied source.

## Preserved snapshot fingerprints

These SHA-256 fingerprints cover the preserved file contents with CRLF line endings normalized to LF, so normal Git line-ending conversion does not change the comparison. They identify the supplied snapshot; they are not a claim about any upstream revision.

| File under `original/` | SHA-256 with LF line endings |
|---|---|
| `.gitignore` | `d5c75e82f2afeceb82a812b9db8d9c4ce961b588704c358b69d78bfe67458afa` |
| `CMakeLists.txt` | `e673bee826a2a0dc10d73c3f2acd663e44730b3729fc6680a495984402e11ca0` |
| `main.cpp` | `8f778a3e8e972fbba5cf5fa558b78cf8d8e791d124f907519b9ebccfde74d369` |
| `Order.h` | `ea5d1b8dea5cc0626d03d3a66d47ce6fe6ff7677efdb7fb07e3cc7ed2daf936d` |
| `OrderBook.h` | `5bf8f4ef6ad4572bb18d1be2d55a6c41ca2837f998e221a82ad0f9ba9816d76b` |
| `README.md` | `4d6c85d3843d9db2bb94e63ad7f2d8269b63acb630e7f577a38150f20b838c91` |
| `Trade.h` | `8b5d1eac30986fd8ff1c03bd796abfc7261682019d97a421982920cce22fac1d` |
