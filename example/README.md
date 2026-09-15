# A small coding task on the e-paper device

`project/battery.sh` contains two threshold mistakes. `project/SPEC.md` defines the
correct behavior, `project/test.sh` contains 14 checks, and `prompt.md` asks fx to fix
the implementation without changing its specification or tests.

After `./esp32-fx prep` and `./esp32-fx load`, insert a FAT microSD card, configure
`.env` as described in the root README, and run:

```sh
./esp32-fx execute --example
```

The command connects Wi-Fi, creates an isolated SD working directory, invokes fx,
verifies that the agent read the project and ran tests before and after its edit,
then independently checks all 14 tests and the unchanged inputs. The answer appears
on the display. Each run starts from the original broken project. Raw results stay
in `private/` and on the card.

The model can read the three fixed files, replace `battery.sh`, and run the fixed
test command. This is the supported coding capability set.

For a workstation test with a deterministic provider and no account or hardware:

```sh
./esp32-fx prep --host-only
```

That runs seven provider rounds through the real fx/WAMR/ACP/tool path, verifies the
repair independently, and renders a framebuffer. It does not call a live model.
