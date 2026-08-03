# Native Bazel build for the ICU4C release fetched in MODULE.bazel. GoogleSQL
# compiles against the header-only aliases below; the frontend binary links the
# four implementation targets explicitly so its ICU data is self-contained.

load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

exports_files(["LICENSE"])

cc_library(
    name = "icu",
    hdrs = glob([
        "source/common/unicode/*.h",
        "source/i18n/unicode/*.h",
        "source/io/unicode/*.h",
    ]),
    defines = ["U_STATIC_IMPLEMENTATION"],
    includes = [
        "source/common",
        "source/i18n",
        "source/io",
    ],
)

alias(
    name = "common",
    actual = ":icu",
)

alias(
    name = "headers",
    actual = ":icu",
)

alias(
    name = "unicode",
    actual = ":icu",
)

cc_library(
    name = "internal_headers",
    hdrs = glob([
        "source/common/*.h",
        "source/i18n/*.h",
        "source/io/*.h",
    ]),
    visibility = ["//visibility:private"],
    deps = [":icu"],
)

cc_library(
    name = "icuuc_lib",
    srcs = glob(["source/common/*.cpp"]),
    copts = ["-DU_COMMON_IMPLEMENTATION"],
    linkstatic = True,
    deps = [":internal_headers"],
)

cc_library(
    name = "icui18n_lib",
    srcs = glob(["source/i18n/*.cpp"]),
    copts = ["-DU_I18N_IMPLEMENTATION"],
    linkstatic = True,
    deps = [":internal_headers"],
)

cc_library(
    name = "icuio_lib",
    srcs = glob(["source/io/*.cpp"]),
    copts = ["-DU_IO_IMPLEMENTATION"],
    linkstatic = True,
    deps = [":internal_headers"],
)

genrule(
    name = "icudt_c",
    srcs = ["source/data/in/icudt76l.dat"],
    outs = ["icudt76_dat.c"],
    cmd = "$(location @//third_party/icu:dat_to_c) " +
          "$(location source/data/in/icudt76l.dat) icudt76 $@",
    tools = ["@//third_party/icu:dat_to_c"],
)

cc_library(
    name = "icudata_lib",
    srcs = [":icudt_c"],
    alwayslink = True,
    linkstatic = True,
    deps = [":icu"],
)
