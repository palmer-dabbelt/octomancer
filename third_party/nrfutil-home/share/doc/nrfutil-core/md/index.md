nrfutil-core documentation
==========================

nRF Util quick guide
====================

nRF Util is the unified command line utility for Nordic products. nRF
Util's `core` module provides the framework and integration for all
commands supported in the utility.

See
[TechDocs](https://docs.nordicsemi.com/bundle/nrfutil/page/README.html)
for complete documentation of nRF Util.

Syntax
------

To see the complete list of subcommands and available options, run
`nrfutil --help`.

Available commands
------------------

nRF Util's functionality is provided through installable and upgradeable
commands that are served on a central package registry on the Internet.

You can list the available commands by using `nrfutil search`.
Alternatively, read [Basic commands and
help](https://docs.nordicsemi.com/bundle/nrfutil/page/guides/getting_started.html).

### Installing nRF Util commands

To install one of nRF Util's commands, run
`nrfutil install <name_of_command>`. For example, to install the
`device` command, run the following command:

    nrfutil install device

### Running nRF Util commands

To run a command you have installed, run `nrfutil <name_of_command>`,
which provides its own set of command line options and features. For
example, `nrfutil device`.

### Upgrading nRF Util commands

To upgrade commands and keep them up to date, run `nrfutil upgrade`.

To list commands that have updates available, use
`nrfutil list --outdated`.

Versioning
----------

nRF Util and all installable commands follow [semantic
versioning](https://semver.org). The API is defined by the visible
command line flags and the JSON output obtained with the `--json` flag.

Getting help
------------

Use `-h` to show the short version of the help text, and `--help` to
show the long version, with more detailed description of each argument,
if available.

In addition to information available in `--help/-h`, all installed
commands have comprehensive documentation available with the built-in
`help` command. For example, `nrfutil device help` would show the
documentation for the `device` command. See also `(...) help --help` for
more viewing options.
