# Fixed protocol evidence

`serialization-vectors.json` and `request-vectors.json` are copied unchanged from the
7.64.0.10819 analysis. They record synthetic inputs executed against the original
machine-code serializer/comparator/CalculateSign, with explicit helper substitutions.
The input binary SHA-256 is recorded in the serialization fixture. Tests compare the
C++ output to those stored bytes rather than generating expectations with the new
implementation. No cookie, key, or account in these files is real.

`request-vectors-8.15.json` covers the current client's title, start and stop
requests. It uses the same synthetic inputs with version 8.15.0.11311 and build
11311. A separate Python calculation reproduced all three historical vectors
before calculating the new URLs, bodies and signed input bytes. The request
builders in the 8.15 DLL establish field placement and signing order. Emulating
its digest routine at RVA `0x795260` on those three inputs produced all three
expected signatures. DLL SHA-256:
`e87378aa3c5aea099ddf15edb1e47f9bb92f6d423c7d84e4f0dc1fc43fc9be08`.

`nav-vector.json` adds the account-specific nav contract: only `appkey`,
`platform`, and `ts` enter the original CalculateSign function before `sign` is
added to the GET query. Its digest was checked by executing that original function
against a synthetic map, separately from the C++ implementation.

Response JSON in `protocol-test.cpp` is synthetic and exercises recovered field
names, failure states, protocol selection, and credential boundaries. Passing tests
does not establish server acceptance or actual OBS broadcasting behavior.

`areas-response.json` is a cookie-free HTTP 200 / code 0 response captured from
`https://api.live.bilibili.com/room/v1/Area/getList` on 2026-11-06. It contains public
category metadata (12 parents, 455 unique child IDs), with no account data. Tests
verify names and mixed numeric/string identifiers against this fixed response;
the application fetches the current list independently at runtime.
