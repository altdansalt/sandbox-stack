#!/usr/bin/env bash
# Assert each guest's behavior; print PASS/FAIL; exit nonzero if any fail.
cd "$(dirname "$0")/.."
fail=0
check() { # name expected_exit actual_exit [expected_out actual_out]
  local n="$1" ee="$2" ae="$3"
  if [ "$ee" != "$ae" ]; then echo "FAIL $n: exit $ae, expected $ee"; fail=1; return; fi
  if [ $# -ge 5 ] && [ "$4" != "$5" ]; then echo "FAIL $n: out '$5', expected '$4'"; fail=1; return; fi
  echo "PASS $n (exit $ae)"
}
out=$(echo "Hello, World" | ./build/rot13/sandbox 2>/dev/null); check "rot13 functional" 0 $? "Uryyb, Jbeyq" "$out"
out=$(printf 'abc\n' | ./build/cat/sandbox 2>/dev/null); check "cat functional" 0 $? "abc" "$out"
./build/evil/sandbox </dev/null >/dev/null 2>&1; check "evil (OOB load traps)" 106 $?
./build/grow/sandbox </dev/null >/dev/null 2>&1; check "grow (OOB past grown mem traps)" 106 $?
./build/escape/sandbox </dev/null >/dev/null 2>&1; check "escape (host refuses fd 3)" 0 $?
./build/escape/sandbox-demo </dev/null >/dev/null 2>&1; check "escape-demo (disallowed syscall SIGKILL)" 137 $?
echo "=== adversarial (sandbox) ==="
declare -A exp=( [positive]=0 [oob_load]=106 [mem_init_oob]=106 [elem_oob]=107 [callind_typemismatch]=109 )
declare -A wz
for m in positive oob_load mem_init_oob elem_oob callind_typemismatch; do
  ./build/adv/$m/sandbox </dev/null >/dev/null 2>&1; check "adv/$m" "${exp[$m]}" $?
done
echo "=== adversarial (wazero control) ==="
for m in positive oob_load mem_init_oob elem_oob callind_typemismatch; do
  ./build/control build/adv/$m.wasm </dev/null >/dev/null 2>&1; e=$?
  case $m in
    positive) exp_wz=0;; *) exp_wz=nonzero;;
  esac
  if [ "$m" = elem_oob ]; then
    # wazero v1.9.0 does NOT trap OOB active element segments; sandbox is stricter
    [ $e -eq 0 ] && echo "NOTE adv/$m: wazero exit 0 (accepts OOB active elem; sandbox traps 107)" || echo "adv/$m wazero exit $e"
  elif [ "$m" = positive ]; then
    check "adv/$m wazero" 0 $e
  else
    [ $e -ne 0 ] && echo "PASS adv/$m wazero (traps/rejects, exit $e)" || { echo "FAIL adv/$m wazero: exit 0"; fail=1; }
  fi
done
[ $fail -eq 0 ] && echo "=== ALL PASS ===" || echo "=== FAILURES ==="
exit $fail
