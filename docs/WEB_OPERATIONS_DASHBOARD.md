# Web Operations Dashboard

## Purpose

This AP-18 slice integrates the browser dashboard with the canonical cross-surface operations contract exposed by `GET /api/operations`.

The browser does not derive safety, governance, incident, maintenance or recovery semantics independently. Those semantics remain defined by the shared AP-17/AP-18 presentation policies and are serialized by `CrossSurfaceOperationsView.hpp`.

## Integration approach

The existing research/runtime dashboard remains intact. `DashboardOperationsOverlay.hpp` injects an additional `Operations` tab into the delivered dashboard HTML.

The overlay is intentionally isolated from the large embedded dashboard asset so the existing research and runtime UI can remain stable while the operations surface evolves independently.

The overlay:

- is injected before the closing `</body>` tag;
- is idempotent and cannot be injected twice;
- consumes only `GET /api/operations`;
- refreshes only while the Operations tab is active;
- performs no local safety or governance classification;
- renders server-provided runtime severity, governance, evidence, maintenance, incident, recovery, workflow, approval and audit state;
- visibly retains fail-closed states such as `STALE`, `FORBIDDEN` and `BLOCKED`.

## Read-only authority

The browser remains read-only.

The operations overlay contains no `POST`, `PUT`, `PATCH` or `DELETE` request and no write endpoint is added by this slice.

The UI cannot:

- enable trading;
- clear a kill switch;
- resume entries;
- approve a governed action;
- acknowledge an incident authoritatively;
- promote a recovery candidate;
- mutate Risk or Execution state;
- synthesize fills;
- override exchange-confirmed execution truth.

The authority value rendered by the browser comes from the server-side operations contract and is expected to remain `READ_ONLY_PRESENTATION`.

## Cross-surface consistency

Terminal and browser now consume the same normalized concepts:

- runtime severity and operator message;
- governance state;
- evidence state;
- maintenance state;
- incident state;
- recovery state;
- pending approval count;
- maintenance / incident / recovery workflow views;
- bounded approval queue;
- bounded audit timeline.

The browser does not duplicate the policy logic used to produce those values.

## Failure UX

If `/api/operations` cannot be read, the browser renders operations evidence as unavailable instead of substituting a healthy default.

Stale evidence remains visibly stale and approval rows retain their server-side fail-closed classifications and statuses.

## Regression coverage

`dashboard_operations_overlay_tests` verifies:

- overlay injection before the closing body tag;
- idempotent injection;
- fallback behavior for partial HTML;
- consumption of `/api/operations`;
- absence of browser write methods.

The test target is attached to the sanitizer build graph so ASan, UBSan and TSan build it before CTest executes the registered test.
