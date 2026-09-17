# Notification Provider Boundary & Delivery Evidence

AP-20 keeps notification delivery outside all trading, safety and execution authority.

## Provider boundary

`NotificationDispatchRequest` contains only notification transport fields: dedup key, alert id, alert generation, channel, audience and attempt number. It exposes `delivery_authorized` but always keeps `execution_authorized = false`.

A provider returns `NotificationDispatchResult` with delivery confirmation, provider reference and failure evidence. Applying that result can only advance the notification delivery state machine to `DELIVERED` or `FAILED`.

A provider result must never acknowledge or resolve an alert, clear a kill switch, resume entries, approve a governed action, mutate Risk/Execution, synthesize fills or override exchange-confirmed execution truth.

## Delivery evidence

`NotificationDeliveryEvidenceRecord` is a presentation/persistence contract for append-only delivery history. Each record contains:

- generation-safe dedup key;
- alert id and generation;
- channel and audience;
- delivery state and attempt;
- terminal flag;
- provider reference;
- failure code/reason;
- optional upstream UTC observation timestamp;
- delivery authority;
- `execution_authorized = false`.

`append_notification_delivery_evidence()` appends only new state evidence and rejects immediate duplicate records. It never rewrites prior evidence.

This is an in-memory contract boundary, not a durable append-only database implementation. Durable persistence, delivery metrics and incident integration belong to the following production-operations work.

## Retry and idempotency

Delivery retries remain bounded by the state machine. The provider boundary does not sleep, schedule or retry by itself. A terminal delivery or terminal failure remains idempotently closed for the same generation-safe dedup key.
