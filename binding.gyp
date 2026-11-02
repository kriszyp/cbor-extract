{
  "targets": [
    {
      "target_name": "extract",
      # scanner_checks.cpp only holds static_asserts: the build fails if the scanner breaks, and the binary is unchanged
      "sources": ["src/extract.cpp", "src/scanner_checks.cpp"],
      # pins the Node-API version instead of following the headers' default; the code needs nothing newer
      "defines": ["NAPI_VERSION=8"],
      # node-gyp's Release already builds with -O3 (/Ox on Windows), without exceptions or RTTI, as C++20: the standard
      # flags below come after node-gyp's, so they win
      "conditions": [
        ["OS=='linux'", {
          "cflags": ["-fvisibility=hidden"],
          "cflags_cc": ["-std=gnu++23"],
          "ldflags": ["-Wl,--as-needed", "-Wl,-z,relro,-z,now"]
        }],
        ["OS=='mac'", {
          "xcode_settings": {
            "OTHER_CPLUSPLUSFLAGS": ["-std=gnu++23"],
            "GCC_SYMBOLS_PRIVATE_EXTERN": "YES",
            "DEAD_CODE_STRIPPING": "YES"
          }
        }],
        ["OS=='win'", {
          "msvs_settings": {
            # VS 2022 has std::expected only under /std:c++latest
            "VCCLCompilerTool": { "AdditionalOptions": ["/std:c++latest"] },
            # common.gypi links with /DEBUG, which turns these off unless Node itself was built with LTCG
            "VCLinkerTool": { "OptimizeReferences": 2, "EnableCOMDATFolding": 2 }
          }
        }]
      ]
    }
  ]
}
