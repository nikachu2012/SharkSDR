#include <SharkSDR/dsp/window.h>
#include <SharkSDR/types.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace SharkSDR::dsp {

const char* windowName(WindowType type) {
    switch (type) {
        case WindowType::Rectangular: return "Rectangular";
        case WindowType::Hann: return "Hann";
        case WindowType::Hamming: return "Hamming";
        case WindowType::Blackman: return "Blackman";
        case WindowType::BlackmanHarris: return "Blackman-Harris";
        case WindowType::Nuttall: return "Nuttall";
        case WindowType::BlackmanHarris7: return "Blackman-Harris 7";
    }
    return "?";
}

std::vector<float> makeWindow(WindowType type, size_t n, bool periodic) {
    std::vector<float> w(n, 1.0f);
    if (n < 2) return w;
    const double m = periodic ? double(n) : double(n - 1);
    // Sum of cosine terms with alternating signs: a0 - a1 cos(x) + a2 cos(2x) - ...
    auto cosine = [&](std::initializer_list<double> a) {
        for (size_t i = 0; i < n; i++) {
            const double x = kTwoPi * double(i) / m;
            double v = 0.0, sign = 1.0;
            int k = 0;
            for (double c : a) {
                v += sign * c * std::cos(double(k++) * x);
                sign = -sign;
            }
            w[i] = float(v);
        }
    };
    switch (type) {
        case WindowType::Rectangular: break;
        case WindowType::Hann: cosine({0.5, 0.5}); break;
        case WindowType::Hamming: cosine({0.54, 0.46}); break;
        case WindowType::Blackman: cosine({0.42, 0.5, 0.08}); break;
        case WindowType::BlackmanHarris: cosine({0.35875, 0.48829, 0.14128, 0.01168}); break;
        case WindowType::Nuttall: cosine({0.355768, 0.487396, 0.144232, 0.012604}); break;
        case WindowType::BlackmanHarris7:
            cosine({0.27105140069342, 0.43329793923448, 0.21812299954311, 0.06592544638803, 0.01081174209837,
                    0.00077658482522, 0.00001388721735});
            break;
    }
    return w;
}

namespace {
// Zeroth order modified Bessel function of the first kind.
double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 64; k++) {
        term *= q / (double(k) * double(k));
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}
}  // namespace

std::vector<float> kaiserWindow(size_t n, double beta) {
    std::vector<float> w(n, 1.0f);
    if (n < 2) return w;
    const double denom = besselI0(beta);
    for (size_t i = 0; i < n; i++) {
        double r = 2.0 * double(i) / double(n - 1) - 1.0;
        w[i] = float(besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / denom);
    }
    return w;
}

}  // namespace SharkSDR::dsp
