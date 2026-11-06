# Fixed protocol evidence

`serialization-vectors.json` and `request-vectors.json` are copied unchanged from the
7.64.0.10819 analysis. They record synthetic inputs executed against the original
machine-code serializer/comparator/CalculateSign, with explicit helper substitutions.
The input binary SHA-256 is recorded in the serialization fixture. Tests compare the
C++ output to those stored bytes rather than generating expectations with the new
implementation. No cookie, key, or account in these files is real.

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
