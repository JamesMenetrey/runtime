// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef DEPS_RESOLVER_H
#define DEPS_RESOLVER_H

#include <vector>

#include "pal.h"
#include "args.h"
#include "trace.h"
#include "fx_definition.h"
#include "deps_format.h"
#include "deps_entry.h"
#include "runtime_config.h"
#include "bundle/runner.h"

// Probe paths to be resolved for ordering
struct probe_paths_t
{
    pal::string_t tpa;
    pal::string_t native;
    pal::string_t resources;
    pal::string_t coreclr;
};

struct deps_resolved_asset_t
{
    deps_resolved_asset_t(const deps_asset_t& asset, const pal::string_t& resolved_path)
        : asset(asset)
        , resolved_path(resolved_path) { }

    deps_asset_t asset;
    pal::string_t resolved_path;
};

typedef std::unordered_map<pal::string_t, deps_resolved_asset_t> name_to_resolved_asset_map_t;

class deps_resolver_t
{
public:
    // if root_framework_rid_fallback_graph is specified it is assumed that the fx_definitions
    // doesn't contain the root framework at all.
    deps_resolver_t(
        const arguments_t& args,
        const fx_definition_vector_t& fx_definitions,
        const deps_json_t::rid_fallback_graph_t* root_framework_rid_fallback_graph,
        bool is_framework_dependent)
        : m_fx_definitions(fx_definitions)
        , m_app_dir(args.app_root)
        , m_host_mode(args.host_mode)
        , m_managed_app(args.managed_application)
        , m_core_servicing(args.core_servicing)
        , m_is_framework_dependent(is_framework_dependent)
        , m_needs_file_existence_checks(false)
    {
        m_fx_deps.resize(m_fx_definitions.size());

        // Process from lowest (root) to highest (app) framework.
        // If we weren't explicitly given a rid fallback graph, that of
        // the root framework is used for higher frameworks.
        int lowest_framework = static_cast<int>(m_fx_definitions.size()) - 1;
        for (int i = lowest_framework; i >= 0; --i)
        {
            pal::string_t deps_file = i == 0
                ? args.deps_path
                : get_fx_deps(m_fx_definitions[i]->get_dir(), m_fx_definitions[i]->get_name());
            trace::verbose(_X("Using %s deps file"), deps_file.c_str());

            if (root_framework_rid_fallback_graph == nullptr && i == lowest_framework)
            {
                m_fx_deps[i] = std::unique_ptr<deps_json_t>(new deps_json_t(false, deps_file, nullptr));

                // The fx_definitions contains the root framework, so set the
                // rid fallback graph that will be used for other frameworks.
                root_framework_rid_fallback_graph = &m_fx_deps[lowest_framework]->get_rid_fallback_graph();
            }
            else
            {
                // The rid graph is obtained from the root framework
                m_fx_deps[i] = std::unique_ptr<deps_json_t>(new deps_json_t(true, deps_file, root_framework_rid_fallback_graph));
            }
        }

        resolve_additional_deps(args.additional_deps_serialized, root_framework_rid_fallback_graph);

        setup_probe_config(args);
    }

    bool valid(pal::string_t* errors)
    {
        for (size_t i = 0; i < m_fx_deps.size(); ++i)
        {
            // Verify the deps file exists. The app deps file does not need to exist
            if (i != 0)
            {
                if (!m_fx_deps[i]->exists())
                {
                    errors->assign(_X("A fatal error was encountered, missing dependencies manifest at: ") + m_fx_deps[i]->get_deps_file());
                    return false;
                }
            }

            if (!m_fx_deps[i]->is_valid())
            {
                errors->assign(_X("An error occurred while parsing: ") + m_fx_deps[i]->get_deps_file());
                return false;
            }
        }

        for (const auto& additional_deps : m_additional_deps)
        {
            if (!additional_deps->is_valid())
            {
                errors->assign(_X("An error occurred while parsing: ") + additional_deps->get_deps_file());
                return false;
            }
        }

        errors->clear();
        return true;
    }

    pal::string_t get_lookup_probe_directories();

    bool resolve_probe_paths(
        probe_paths_t* probe_paths,
        std::unordered_set<pal::string_t>* breadcrumb,
        bool ignore_missing_assemblies = false);

    const deps_json_t& get_root_deps() const
    {
        return *m_fx_deps[m_fx_definitions.size() - 1];
    }

    void enum_app_context_deps_files(std::function<void(const pal::string_t&)> callback);

    bool is_framework_dependent() const
    {
        return m_is_framework_dependent;
    }

    void get_app_dir(pal::string_t *app_dir) const
    {
        if (m_host_mode == host_mode_t::libhost)
        {
            static const pal::string_t s_empty;
            *app_dir = s_empty;
            return;
        }
        *app_dir = m_app_dir;
        if (m_host_mode == host_mode_t::apphost)
        {
            if (bundle::info_t::is_single_file_bundle())
            {
                const bundle::runner_t* app = bundle::runner_t::app();
                if (app->is_netcoreapp3_compat_mode())
                {
                    *app_dir = app->extraction_path();
                }
            }
        }

        // Make sure the path ends with a directory separator
        // This has been the behavior for a long time, and we should make it consistent
        // for all cases.
        if (app_dir->back() != DIR_SEPARATOR)
        {
            app_dir->append(1, DIR_SEPARATOR);
        }
    }

private:
    void setup_shared_store_probes(
        const arguments_t& args);

    void setup_probe_config(
        const arguments_t& args);

    void init_known_entry_path(
        const deps_entry_t& entry,
        const pal::string_t& path);

    void resolve_additional_deps(
        const pal::string_t& additional_deps_serialized,
        const deps_json_t::rid_fallback_graph_t* rid_fallback_graph);

    const deps_json_t& get_app_deps() const
    {
        return *m_fx_deps[0];
    }

    static pal::string_t get_fx_deps(const pal::string_t& fx_dir, const pal::string_t& fx_name)
    {
        pal::string_t fx_deps = fx_dir;
        pal::string_t fx_deps_name = fx_name + _X(".deps.json");
        append_path(&fx_deps, fx_deps_name.c_str());
        return fx_deps;
    }

    // Resolve order for TPA lookup.
    bool resolve_tpa_list(
        pal::string_t* output,
        std::unordered_set<pal::string_t>* breadcrumb,
        bool ignore_missing_assemblies);

    // Resolve order for culture and native DLL lookup.
    bool resolve_probe_dirs(
        deps_entry_t::asset_types asset_type,
        pal::string_t* output,
        std::unordered_set<pal::string_t>* breadcrumb);

    // Probe entry in probe configurations and deps dir.
    bool probe_deps_entry(
        const deps_entry_t& entry,
        const pal::string_t& deps_dir,
        int fx_level,
        pal::string_t* candidate,
        bool &found_in_bundle);

private:
    const fx_definition_vector_t& m_fx_definitions;

    // Resolved deps.json for each m_fx_definitions (corresponding indices)
    std::vector<std::unique_ptr<deps_json_t>> m_fx_deps;

    pal::string_t m_app_dir;

    // Mode in which the host is being run. This can dictate how dependencies should be discovered.
    const host_mode_t m_host_mode;

    // The managed application the dependencies are being resolved for.
    pal::string_t m_managed_app;

    // Servicing root, could be empty on platforms that don't support or when errors occur.
    pal::string_t m_core_servicing;

    // Special entry for coreclr path
    pal::string_t m_coreclr_path;

    // Custom deps files for the app
    std::vector< std::unique_ptr<deps_json_t> > m_additional_deps;

    // Various probe configurations.
    std::vector<probe_config_t> m_probes;

    // Is the deps file for an app using shared frameworks?
    const bool m_is_framework_dependent;

    // File existence checks must be performed for probed paths.This will cause symlinks to be resolved.
    bool m_needs_file_existence_checks;
};

#endif // DEPS_RESOLVER_H
