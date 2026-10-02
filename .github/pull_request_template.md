## Summary

- 

## Type of change

- [ ] Protocol / PTTY framing
- [ ] Core server / sessions
- [ ] psh shell / builtins
- [ ] TTY backend (FreeBSDPTY / PipeTTY)
- [ ] PS5 platform
- [ ] PS4 platform
- [ ] Host client
- [ ] External execution
- [ ] Tests
- [ ] Build / CI / release
- [ ] Documentation
- [ ] Bug fix

## Risk surface

- Affected platform(s):
- Affected commands, protocol messages, or workflows:
- Console safety considerations:

## Verification

- [ ] `cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Debug`
- [ ] `cmake --build build-host -j`
- [ ] `ctest --test-dir build-host --output-on-failure`
- [ ] `python3 tests/integration/test_e2e.py --build build-host`
- [ ] PS5 payload builds with `prospero.cmake`
- [ ] PS4 payload builds with `orbis.cmake`
- [ ] PS5 manual check
- [ ] PS4 manual check
- [ ] Not applicable, because:

## Notes for reviewers

- 

## Scope confirmation

- [ ] This change supports legitimate homebrew, debugging, research, and offline development on consoles the user owns.
- [ ] Every PS4/PS5 runtime claim is marked `HARDWARE TEST REQUIRED` unless it was actually tested on hardware.
