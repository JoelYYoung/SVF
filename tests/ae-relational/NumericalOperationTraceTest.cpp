#include "AE/Core/NumericalOperationTrace.h"
#include "AE/Core/Expression.h"
#include "AE/Core/NumericalDomainFactory.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace AD = SVF::AbstractDomain;

namespace
{

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::unique_ptr<AD::NumericalDomain> cloneNumerical(
    const AD::NumericalDomain& state)
{
    std::unique_ptr<AD::AbstractDomain> clone = state.clone();
    return std::unique_ptr<AD::NumericalDomain>(
        static_cast<AD::NumericalDomain*>(clone.release()));
}

AD::DomainKind requestedDomain()
{
    const char* requested = std::getenv("SVF_TRACE_TEST_DOMAIN");
    if (!requested || std::string(requested) == "box")
        return AD::DomainKind::Box;
    if (std::string(requested) == "octagon")
        return AD::DomainKind::Octagon;
    if (std::string(requested) == "polyhedra")
        return AD::DomainKind::ConvexPolyhedra;
    throw std::invalid_argument(
        "SVF_TRACE_TEST_DOMAIN must be box, octagon, or polyhedra");
}

} // namespace

int main()
{
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const char* requestedPath = std::getenv("SVF_TRACE_TEST_OUTPUT");
    const bool preserve = requestedPath && *requestedPath;
    const std::filesystem::path path = preserve
        ? std::filesystem::path(requestedPath)
        : std::filesystem::temp_directory_path() /
              ("svf-ae-numerical-trace-" + std::to_string(stamp) + ".tsv");

    try
    {
        const AD::DomainKind domain = requestedDomain();
        {
            auto writer = std::make_shared<AD::NumericalOperationTraceWriter>(
                path.string());
            std::unique_ptr<AD::NumericalDomain> state =
                AD::traceNumericalDomain(
                    AD::makeNumericalDomain(domain, false),
                    writer);
            const AD::Variable x(1);
            state->assign(x, AD::LinearExpression(AD::Rational(7)));
            state->assume(
                AD::greaterEqual(AD::LinearExpression(x),
                                 AD::LinearExpression(AD::Rational(0))));
            require(state->bound(x) == AD::Interval::singleton(AD::Rational(7)),
                    "tracing changed the wrapped bound");

            std::unique_ptr<AD::NumericalDomain> copy = cloneNumerical(*state);
            copy->forget(x);
            state->joinWith(*copy);
            state->project({x});
            require(state->isTop(), "tracing changed lattice semantics");
        }

        std::ifstream input(path);
        require(static_cast<bool>(input), "trace file was not created");
        std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
        require(contents.find("SVF-AE-NUMERICAL-TRACE\t1") != std::string::npos,
                "trace version header is missing");
        require(contents.find("\tcreate\t") != std::string::npos,
                "trace create event is missing");
        require(contents.find("\tclone\t") != std::string::npos,
                "trace clone event is missing");
        require(contents.find("\tdestroy\t") != std::string::npos,
                "trace destroy event is missing");
        require(contents.find("\tassign-linear\t") != std::string::npos,
                "trace assignment event is missing");
        require(contents.find("\tjoin\t") != std::string::npos,
                "trace binary lattice event is missing");
        require(contents.find("\tbound-variable\t") != std::string::npos,
                "trace query event is missing");
        if (!preserve)
            std::filesystem::remove(path);
        std::cout << "NumericalOperationTraceTest: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        if (!preserve)
            std::filesystem::remove(path);
        std::cerr << "NumericalOperationTraceTest: FAIL: " << error.what()
                  << '\n';
        return 1;
    }
}
