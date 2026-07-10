# GreetingStore Error Handling Design

## Goal

Make `GreetingStore::GreetingFor` uphold its documented no-throw fallback
contract for all Halcyon query, cursor-fetch, and row-mapping failures, and
replace the placeholder Halcyon dependency URL in the README.

## Scope

- Update only the greeting lookup error path and its focused tests.
- Keep the public `GreetingStore` API and successful lookup behavior unchanged.
- Keep the existing `BUILD_DB2_TESTS` gate.
- Do not depend on a sibling Halcyon source checkout or a live Db2 instance for
  the new regression cases.

## Production Behavior

`GreetingStore::GreetingFor` continues to issue the existing case-insensitive
query and return the first salutation when the query succeeds.

For a returned row, it uses Halcyon's non-throwing `Row::try_as<std::string>()`.
If column retrieval or conversion fails, it logs the Halcyon error and returns
`"Hello"`.

If result-set iteration ends because `fetch()` failed, it checks
`QueryResult::ok()`, logs the cursor error, and returns `"Hello"`. A clean empty
result remains a normal miss and returns `"Hello"` without an error log.

## Test Design

Add a small, repository-local `GreetingStoreTestDriver` under
`tests/greeting/`. It implements Halcyon's `ICliDriver` interface with only the
state needed to script one-row queries, row-mapping errors, and fetch errors.
This keeps the tests self-contained while exercising Halcyon's real
`Database -> QueryResult -> Row` path.

Tests construct `GreetingStore` through a narrowly scoped private test peer.
The peer is declared as a friend but does not add a public production factory.

Regression coverage verifies:

1. A valid row still returns its salutation.
2. A nullable or otherwise unmappable salutation does not throw and returns
   `"Hello"`.
3. A cursor fetch error does not throw and returns `"Hello"`.

The existing live Db2 test remains available when `DB2_CONN_STR` is set.

## Documentation

Change the Halcyon link in `Readme.md` from the GitHub homepage to
`https://github.com/YH-Hung/Halcyon`.

## Verification

Configure with `BUILD_DB2_TESTS=ON`, build `greeting_store_tests`, and first run
the new tests against the unfixed implementation to confirm the mapping and
fetch cases fail for the expected reasons. After the production fix, rerun the
focused target and then the complete CTest suite with `DB2_CONN_STR` unset.
