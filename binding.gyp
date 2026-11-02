{
  "targets": [
    {
      "target_name": "extract",
      "sources": ["src/extract.cpp"],
      # pins the Node-API version instead of following the headers' default; the code needs nothing newer
      "defines": ["NAPI_VERSION=8"],
      # node-gyp's Release already builds with -O3 (/Ox on Windows), without exceptions or RTTI
      "conditions": [
        ["OS=='linux'", {
          "cflags": ["-fvisibility=hidden"],
          "ldflags": ["-Wl,--as-needed", "-Wl,-z,relro,-z,now"]
        }],
        ["OS=='mac'", {
          "xcode_settings": {
            "GCC_SYMBOLS_PRIVATE_EXTERN": "YES",
            "DEAD_CODE_STRIPPING": "YES"
          }
        }],
        ["OS=='win'", {
          # common.gypi links with /DEBUG, which turns these off unless Node itself was built with LTCG
          "msvs_settings": { "VCLinkerTool": { "OptimizeReferences": 2, "EnableCOMDATFolding": 2 } }
        }]
      ]
    }
  ]
}
