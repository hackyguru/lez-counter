#!/usr/bin/env bash
# Read the shared counter straight from the LEZ testnet — no Basecamp, no module,
# no wallet. Anyone, anywhere can run this to check what the module shows.
#   ./verify.sh            print the current value
#   ./verify.sh watch      print it again whenever it changes
set -euo pipefail
SEQ="${SEQ:-https://testnet.lez.logos.co}"
PROGRAM="${PROGRAM:-473ERB4qt4as8KUbAvc9gm6hWxb6Mt7q3eANnsNLx4MN}"
COUNTER="${COUNTER:-FkVJaxkLU7mWmndxVQoKjVE2e34wWBwoXUxdcynSNYcw}"

read_value() {
    curl -s -m 15 -X POST "$SEQ" -H 'content-type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccount\",\"params\":[\"$COUNTER\"]}" |
    python3 -c "
import json, sys
r = json.load(sys.stdin)['result']
shard = r['data']['shards'].get('$PROGRAM', [])
print(int.from_bytes(bytes(shard), 'little') if shard else 0)"
}
block() {
    curl -s -m 15 -X POST "$SEQ" -H 'content-type: application/json' \
        -d '{"jsonrpc":"2.0","id":1,"method":"getLastBlockId","params":[]}' |
    python3 -c "import json,sys; print(json.load(sys.stdin)['result'])"
}

if [ "${1:-}" = "watch" ]; then
    last=""
    while true; do
        v=$(read_value)
        [ "$v" != "$last" ] && echo "$(date +%T)  block $(block)  counter = $v" && last="$v"
        sleep 5
    done
fi
echo "counter = $(read_value)   (block $(block), program $PROGRAM)"
