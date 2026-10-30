# Contributing to ChooChooTracker

Pull requests are welcome.

ChooChooTracker is built with AI-assisted and agent-assisted development in mind. Humans, coding agents, and mixed workflows are all welcome here. The important part is that a change is understandable, tested, and useful to people making music on the device.

## What fits the project

ChooChooTracker is a handheld groovebox and tracker. Keep changes focused on fast music-making with a small, readable codebase. Good contributions improve sequencing, sound design, performance controls, file compatibility, portability, or the practical workflow of the instrument. New sound engines are welcome.

Please do not turn it into a conventional DAW. Features that need a mouse-first workflow, a large arrangement view, or deep studio-style editing probably belong somewhere else. Avoid 1:1 copies of features from other trackers. We aren't a clone even though all trackers sort of look the same.

## Before opening a pull request

- Feel free to visit on the Discord, discuss with other contributors or project owner.
- Keep the change small and focused. One problem per pull request is ideal. 
- Preserve the MIT license. Do not copy code or assets with incompatible terms.
- Add or update tests for behavior changes. Run `make -f Makefile.test -j4` from `tracker` for engine or playback changes.
- Update `docs/USER_MANUAL.md` when a user-facing behavior changes.
- When changing the user manual, use the `choochootracker-user-manual` skill so it stays clear and useful to musicians.
- Explain what changed, how you tested it, and any limitations in the pull request description.
- Test your changes on real hardware. Windows is good, Portmaster/Android are better. Try to break your own code in controlled environment before users do it in the wild.
- Please exclude AI "docs" and other non necessary files from your PR unless you know exactly what you are doing.

## Working with agents

### General considerations

If an agent wrote some or all of a change, that is fine. Review the result before submitting it, keep the diff narrow, and include the same tests and context you would for handwritten code. A clear pull request is more useful than a claim about who wrote it. Do not include agent generated doc such as project architecture, etc. We already have it. Keep your agents.md to yourself.

Check your DSP for optimisation: no unecessary preallocations, prefer LUTs or approximations to compute hungry functions like tanhf, prefer lazy updates. When in doubt, compile two versions and benchmark thm.

Thorough human testing is strongly encouraged. Hunt for edge cases, do stupid things. If it breaks, it's probably not ready for PR. We strongly recommend you actually make music with your contributions before you send them.

### Designing sound engines with agents

AI Agents can come up with very interesting synth algos if you guide then into what you want. Not everything has to be pulled from a Github project. You can use tutorials, research papers, VCV patches, etc. as a prompt complement.

Bogie, Sintered and MME algos are all AI designs. Phaser/flanger/chorus and Destruction are also AI designs.

Sometimes its simpler to have the AI design something than reusing a FOSS project.

When using an agent, I found I had more interesting results if I ask for the agent to have the synth/effect parameters extend WAY beyond what is normal in music production. I don't know why, maybe agents will tend to think we only use the values they read in synth tutorials? anyway, here is a useful prompt "0% to half is normal range. above 50% and up to 100%, its extended range. Parameter can have extreme impact in this range. Extended range good for experiments and interesting sound design"

## Note on designing instruments

An instrument must expose parameters in its own screen, but also modulation destinations and lane FX. Don't forget these.

Make sure the output volume is similar to the other instruments.
Gain compensation may be required. See the measurement documentation in `docs/`.

There are two types of instruments:
- VCOs, which reuse the standard filter/ADSR chain (such as PCM samples, Braids, and Plaits)
- Voices, which have their own post-VCO chain (such as AY, aChChid, etc.)

An engine can have different type of models, you can design a family of instruments (cf: Braids, Plaits, MME, etc.)

Do not expose all parameters of a complicated synth.
Macro controls can do a lot. Favor immediacy.
A parameter can work differently accross its range, or exhibit different behaviors at its extreme values.

You are designing a music interface, it must be intuitive and incite discovery.

## Note on designing effects

Read above note on synth, a lot of it translates.

Effects are insert/masters, or sends. Reverb and delay are sends, the others are insert/masters.

Effects have 8 parameters, fixed. Don't add new lane FXs for your effect, reuse the existing ones. 

## Getting started

Clone with submodules (Ableton Link and Asio are pulled in as git submodules
rather than vendored):

```sh
git clone --recurse-submodules https://github.com/<you>/ChooChooTracker.git
# or, for an existing checkout:
git submodule update --init chipnomad_lib/external/ableton_link chipnomad_lib/external/asio
```

### Ableton Link

The Ableton Link submodule is required only for authorised official
ChooChooTracker builds. This repository, its MIT license and its releases do
not grant a license to Ableton Link. The authorisation held for official
ChooChooTracker releases does not extend to forks, derivative works or other
third-party distributions; their maintainers must obtain their own
authorisation from Ableton before building or distributing a product that
includes Ableton Link.

Open an issue if you want to discuss a larger idea before writing code. For a small fix, a pull request with a short description is usually enough.

Thank you for helping keep ChooChooTracker focused, playable, and fun.


