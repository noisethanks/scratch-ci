#include "Source.hpp"

#include <format>

#include "SpringChain.hpp"

namespace hyprtail::source {
    namespace {
        std::vector<params::SDecl> parseDecls(std::initializer_list<std::string> texts) {
            std::vector<params::SDecl> out;
            for (const auto& t : texts) {
                auto d = params::parseDecl(t);
                if (d)
                    out.push_back(std::move(*d));
            }
            return out; // the texts are fixed; unit tests check none was dropped
        }

        const std::vector<params::SDecl>& springDecls() {
            static const auto d = parseDecls({
                std::format("float mass {} 0.01 100", spring::MASS),
                std::format("float stiffness {} 1 1000000", spring::STIFFNESS),
                std::format("float damping {} 0 10000", spring::DAMPING),
                std::format("float age_step_ms {} 0 1000", spring::AGE_STEP_MS),
            });
            return d;
        }
    }

    bool known(std::string_view kind) {
        return kind == "pointer" || kind == "spring";
    }

    std::string kindList() {
        return "pointer, spring";
    }

    const std::vector<params::SDecl>& decls(std::string_view kind) {
        static const std::vector<params::SDecl> none;
        return kind == "spring" ? springDecls() : none;
    }

    std::unique_ptr<ISource> make(std::string_view kind, size_t capacity, uint64_t seedBase) {
        if (kind == "spring")
            return std::make_unique<CSpringChainSource>(capacity, seedBase);
        return std::make_unique<CTrailRing>(capacity, seedBase);
    }

    std::map<std::string, double> resolve(std::string_view kind, const std::map<std::string, std::string>& defaults, const std::map<std::string, std::string>& overrides,
                                          std::string& problems) {
        std::vector<std::pair<params::SDecl, params::SValue>> all;
        for (const auto& d : decls(kind))
            all.emplace_back(d, d.def);

        params::applyOverrides(all, defaults, "source", problems);
        params::applyOverrides(all, overrides, "source", problems);

        std::map<std::string, double> out;
        for (const auto& [decl, value] : all)
            if (const auto s = params::scalar(value))
                out[decl.name] = *s;
        return out;
    }
}
