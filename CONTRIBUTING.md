# How to become a contributor and submit your own code (WIP)

## Contributor License Agreements

We'd love to accept your sample apps and patches! Before we can take them, we
have to jump a couple of legal hurdles.

Please fill out either the individual or corporate Contributor License Agreement
(CLA).

- If you are an individual writing original source code and you're sure you
  own the intellectual property, then you'll need to sign an individual CLA.
- If you work for a company that wants to allow you to contribute your work,
  then you'll need to sign a corporate CLA.

Contact <office@ondewo.com> to receive the appropriate CLA and instructions for
how to sign and return it. Once we receive it, we'll be able to accept your pull
requests.

## Before you open a pull request here

**Almost nothing in this repository is written by hand.** `api/`, `public-api.h`, `CMakeLists.txt` and
`ondewo-client-config.cmake.in` are all produced by the `ondewo-cpp-proto-compiler` docker image. Editing them
directly is never the fix - the next `make build` overwrites the change, and the pre-commit hooks exclude them
so no formatter creates a spurious diff either.

Route your change to the repository that owns it:

| You want to change                          | Open the pull request against                                          |
| ------------------------------------------- | ---------------------------------------------------------------------- |
| a message, a field or an RPC                | [ondewo-t2s-api](https://github.com/ondewo/ondewo-t2s-api)                     |
| how the C++ stubs or the CMake package look | [ondewo-proto-compiler](https://github.com/ondewo/ondewo-proto-compiler) (the `cpp/` target) |
| the build, the tests, the docs, the release | this repository                                                        |

A change in either submodule reaches this client by bumping its pin in the Makefile's Variables chapter
(`ONDEWO_T2S_API_GIT_BRANCH`, `ONDEWO_PROTO_COMPILER_GIT_BRANCH`) and regenerating.

## Contributing A Patch

1. Submit an issue describing your proposed change to the repo in question.
1. The repo owner will respond to your issue promptly.
1. If your proposed change is accepted, and you haven't already done so, sign a
   Contributor License Agreement (see details above).
1. Fork the desired repo, develop and test your code changes.
1. Ensure that your code adheres to the existing style in the sample to which
   you are contributing. Refer to the
   [Google Cloud Platform Samples Style Guide](https://cloud.google.com/community/tutorials/styleguide) for the
   recommended coding standards for this organization.
1. Ensure that your code has an appropriate set of unit tests which all pass.
1. Submit a pull request.

## Local checks

```shell
make setup_developer_environment_locally   ## Submodules + pre-commit hooks
make build                                 ## Regenerate the stubs and build the library
make test                                  ## check_build + the CMake consumption smoke test
make precommit_hooks_run_all_files         ## Run every pre-commit hook over the whole tree
```

Commit messages follow [Conventional Commits](https://www.conventionalcommits.org/) (`feat: …`, `fix(scope): …`,
`docs: …`). Do **not** prepend the JIRA ticket yourself - the `giticket` pre-commit hook reads it from the
branch name and prepends `[<ticket>]` on commit, so writing it by hand produces a duplicated prefix.
