# aap-clap-hosting-helper: AAP hosting helper that treats AAP plugins as CLAP plugins

## What is this?

aap-clap-hosting helper is an library that eases AAP hosting by creating a
fake CLAP entrypoints for each AAP plugin service.

Everything is implemented by AI with a lot of sessions from atsushieno.

CLAP does not support Android and it is not suitable for mobile platforms,
but some people only want to deal with some known API such as CLAP API.

aap-juce exists exactly for that purpose and provides support AAP hosting
within JUCE API (juce_audio_plugin_client) by making best use of
`juce::AudioPluginFormat` abstraction.

We also have [atsushieno/uapmd](https://github.com/atsushieno/uapmd) for
easier cross-platform plugin host development for desktop, mobile, and Web.

Those CLAP enthusiasts who refuse to use all above can still use this library.

## Features

The following AAP features are exposed as CLAP API:

- parameters
- states
- presets
- mapping from UMP to CLAP events

This library doesn't expose features that don't exist on AAP (such as
threadpool).

## Usage

`aap-clap-hosting-helper.h` exposes some functions that enumerates
`aap_clap_hosting_helper_entry_t` which almost works like `clap_entry_t`.
You can use `get_factory()` to retrieve either a plugin factory or a preset
factory. Through the plugin factory, you can manipulate AAP plugins as if
they were CLAP plugins.

For further details, see [samples/clap-host](samples/clap-host) to learn
how you can build AAP host apps using this pseudo CLAP API.

## Development

Since this is not really what I want to achieve (note that I am primarily
working on the AAP API, not for CLAP), this library is basically left
unsupported as long as it is an unpaid work.
Issue reports without PRs are likely left as is.
