# Binance Testnet authenticated user data stream

Sentum receives exchange-confirmed account and order events through the Spot
Testnet WebSocket API at `wss://ws-api.testnet.binance.vision/ws-api/v3`.
The retired REST listen-key creation, renewal, and deletion workflow is not used.

## Authentication and event correlation

After TLS peer and hostname verification and WebSocket establishment, the client
sends `userDataStream.subscribe.signature`. The execution client supplies a fresh
timestamp and HMAC-SHA256 signature. Signing includes the alphabetically ordered
`apiKey`, `recvWindow`, and `timestamp` parameters, joined as UTF-8 `name=value`
pairs with `&`. The signature is encoded as hexadecimal; the WebSocket request
identifier and method name are outside the signed parameters.

A connected socket or running worker does not establish subscription health.
Health becomes true only after a response with the matching request identifier,
integer status `200`, and a nonnegative integer `result.subscriptionId`. Every
explicit restart uses a new request identifier and requires a new acknowledgement.

Order and account events arrive inside an `event` object with the matching
`subscriptionId`. Only the unwrapped authenticated event reaches order processing.
Raw legacy events, malformed payloads, mismatched subscription identifiers,
unexpected acknowledgements, and callback failures invalidate the stream.
`eventStreamTerminated` invalidates its subscription immediately. A connection
`serverShutdown` event also invalidates the stream, without requiring a
subscription identifier.

## Health and recovery

The transport exposes separate worker-lifecycle and authenticated-health states.
Connection establishment and subscription acknowledgement each have a 10-second
deadline. Once subscribed, 60 seconds without transport activity invalidates
health. Binance sends WebSocket ping frames every 20 seconds; receipt refreshes
the activity deadline and preserves automatic pong replies. An idle account
therefore stays healthy without account or order events.

Disconnect, rejection, timeout, termination, and invalid payloads clear health
before notifying the session. Late acknowledgements cannot restore a failed
connection. The stream stops on failure and does not silently reconnect. Its
owner must explicitly restart authentication and complete order reconciliation
before reopening entry admission. A successful new subscription alone does not
prove that order updates missed during the interruption have been recovered.

Health and event callbacks run outside internal lifecycle and state locks.
Callbacks may request stop; worker joining remains the responsibility of an
external owner. Shutdown clears health, cancels the endpoint, and joins the
worker before releasing transport resources.

## Validation

`user_data_subscription` covers protocol acknowledgement correlation, rejection,
malformed messages, termination, and stale responses. Local TLS tests exercise
actual WebSocket authentication exchange, event unwrapping, abrupt disconnect,
explicit reconnect, connection and subscription deadlines, idle-account
ping/pong activity, silent connection expiry, callback-initiated stop, and TLS
hostname rejection before subscription credentials are transmitted. These tests
use generated local certificates and fixture credentials and do not contact an
exchange or submit orders.

The existing `websocket_lifecycle` suite additionally qualifies concurrent
start/stop, preparation cancellation, failed startup, and callback/owner joining.

## Protocol references

- [Binance Spot Testnet WebSocket API](https://developers.binance.com/en/docs/products/spot/testnet/web-socket-api): endpoint, HMAC signing, subscription acknowledgements, and ping/pong behavior.
- [Binance Spot Testnet User Data Streams](https://developers.binance.com/en/docs/products/spot/testnet/user-data-stream): event envelopes and stream termination.
- [Binance Spot Testnet changelog](https://github.com/binance/binance-spot-api-docs/blob/master/testnet/CHANGELOG.md): retirement of the REST listen-key endpoints on February 4, 2026.
