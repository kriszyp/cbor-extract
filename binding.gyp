{
  "targets": [
    {
      "target_name": "extract",
      "sources": ["src/extract.cpp"],
      "conditions": [
        ["OS=='linux'", {
          "cflags": ["-O3", "-fvisibility=hidden"],
          "cflags_cc": ["-fvisibility=hidden", "-fvisibility-inlines-hidden"]
        }]
      ]
    }
  ]
}
