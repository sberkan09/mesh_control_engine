- Windows test fixes, M-PLATFORM-CI step 4: `.gitattributes` checks text out with LF (the byte-for-byte tests
  `import_demo_reimport` and the scene round trip failed on CRLF checkouts); `cli_test` runs its fake `mce_mcp` through
  a `.cmd` on Windows; `run_transcript.py` reads server pipes with a thread there (`select` takes only sockets); the
  terrain editor test closes its stream before undo rewrites the file.
