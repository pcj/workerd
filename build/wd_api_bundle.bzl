load("@capnp-cpp//src/capnp:cc_capnp_library.bzl", "cc_capnp_library")

CAPNP_TEMPLATE = """@{id};

using Modules = import "/workerd/jsg/modules.capnp";

const {const_name} :Modules.Bundle = (
  modules = [
{modules}
]);
"""

MODULE_TEMPLATE = """(name = "{name}", src = embed "{path}")"""

def _relative_path(file_path, dir_path):
    if not file_path.startswith(dir_path):
        fail("file_path need to start with dir_path: " + file_path + " vs " + dir_path)
    return file_path.removeprefix(dir_path)

def gen_api_bundle_capnpn_impl_(ctx):
    output_dir = ctx.outputs.out.dirname + "/"

    modules = [MODULE_TEMPLATE.format(
        name = ctx.attr.modules[m],
        # capnp doesn't allow ".." dir escape, make paths relative.
        # this won't work for embedding paths outside of rule directory subtree.
        path = _relative_path(
            ctx.expand_location("$(location {})".format(m.label), ctx.attr.data),
            output_dir,
        ),
    ) for m in ctx.attr.modules]

    content = CAPNP_TEMPLATE.format(
        id = ctx.attr.id,
        modules = ",\n".join(modules),
        const_name = ctx.attr.const_name,
    )
    ctx.actions.write(ctx.outputs.out, content)

gen_api_bundle_capnpn = rule(
    implementation = gen_api_bundle_capnpn_impl_,
    attrs = {
        "id": attr.string(mandatory = True),
        "out": attr.output(mandatory = True),
        "modules": attr.label_keyed_string_dict(allow_empty = False, allow_files = True),
        "data": attr.label_list(allow_files = True),
        "const_name": attr.string(mandatory = True),
    },
)

def wd_api_bundle(name, id, const_name, modules, **kwargs):
    gen_api_bundle_capnpn(
        name = name + "@capnp",
        out = name + ".capnp",
        id = id,
        const_name = const_name,
        modules = modules,
        data = [m for m in modules],
    )

    cc_capnp_library(
        name = name + "",
        srcs = [name + ".capnp"],
        strip_include_prefix = "",
        visibility = ["//visibility:public"],
        data = [m for m in modules],
        deps = ["//src/workerd/jsg:modules_capnp"],
        **kwargs,
    )
