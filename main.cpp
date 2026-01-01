#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <windows.h>
#include <commdlg.h>
#include <fcntl.h>
#include <io.h>

#include <aubio/aubio.h>

namespace fs = std::filesystem;
using namespace std; // sorry lol

const double SYSTEM_LATENCY_MS = -27.0; // this works the most, but sometime might not be true tho

class StderrSilencer {
    int saved_stderr;
    int dev_null;
public:
    StderrSilencer() {
        saved_stderr = _dup(_fileno(stderr));
        dev_null = _open("NUL", _O_WRONLY);
        _dup2(dev_null, _fileno(stderr));
    }
    ~StderrSilencer() {
        _dup2(saved_stderr, _fileno(stderr));
        _close(dev_null);
    }
};

// --- DATA COLLECTION ---

vector<double> get_onsets(const wstring& file_path) {
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, file_path.c_str(), (int)file_path.length(), NULL, 0, NULL, NULL);
    string path_str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, file_path.c_str(), (int)file_path.length(), &path_str[0], size_needed, NULL, NULL);

    StderrSilencer silencer; // aubio spams stderr like crazy, gagging it here
    uint_t samplerate = 44100;
    uint_t win_s = 1024;
    uint_t hop_s = 256; 
    aubio_source_t* source = new_aubio_source(path_str.c_str(), samplerate, hop_s);
    if (!source) return {};

    aubio_onset_t* onset = new_aubio_onset("specdiff", win_s, hop_s, samplerate);
    aubio_onset_set_threshold(onset, 0.30); 

    fvec_t* in = new_fvec(hop_s);
    fvec_t* out = new_fvec(1);
    vector<double> onsets;
    uint_t read = 0;
    do {
        aubio_source_do(source, in, &read);
        aubio_onset_do(onset, in, out);
        if (out->data[0] != 0) onsets.push_back(aubio_onset_get_last_ms(onset));
    } while (read == hop_s);

    del_aubio_onset(onset);
    del_aubio_source(source);
    del_fvec(in);
    del_fvec(out);
    return onsets;
}

vector<double> get_flux(const wstring& file_path, double& out_sample_period) {
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, file_path.c_str(), (int)file_path.length(), NULL, 0, NULL, NULL);
    string path_str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, file_path.c_str(), (int)file_path.length(), &path_str[0], size_needed, NULL, NULL);

    StderrSilencer silencer;
    uint_t samplerate = 44100;
    uint_t win_s = 512;
    uint_t hop_s = 128;  
    out_sample_period = (double)hop_s / samplerate;

    aubio_source_t* source = new_aubio_source(path_str.c_str(), samplerate, hop_s);
    if (!source) return {};

    aubio_pvoc_t* pvoc = new_aubio_pvoc(win_s, hop_s);
    aubio_specdesc_t* specdesc = new_aubio_specdesc("specflux", win_s);
    cvec_t* fft_grain = new_cvec(win_s);
    fvec_t* in = new_fvec(hop_s);
    fvec_t* out = new_fvec(1);
    
    vector<double> flux;
    uint_t read = 0;
    do {
        aubio_source_do(source, in, &read);
        aubio_pvoc_do(pvoc, in, fft_grain);
        aubio_specdesc_do(specdesc, fft_grain, out);
        flux.push_back(out->data[0]);
    } while (read == hop_s);

    del_aubio_specdesc(specdesc);
    del_aubio_pvoc(pvoc);
    del_aubio_source(source);
    del_fvec(in);
    del_fvec(out);
    del_cvec(fft_grain);
    return flux;
}

// --- SCOUTING & VOTING ---

double scout_bpm_range(const vector<double>& flux, double sample_period) {
    int N = flux.size();
    // straight up abusing autocorrelation (acf) here.
    // just sliding the signal over itself to find the strongest "thump" period.
    double mean = 0; for(double v : flux) mean += v; mean /= N;
    vector<double> norm = flux; for(double &v : norm) v -= mean;
    double max_acf = -1;
    int best_lag = 0;
    // clamping lag because looking for <60bpm or >320bpm is a waste of cpu
    int min_lag = (int)(0.1875 / sample_period); 
    int max_lag = (int)(1.0 / sample_period);
    if (max_lag >= N) max_lag = N - 1;
    for (int lag = min_lag; lag < max_lag; ++lag) {
        double sum = 0;
        for (int i = 0; i < N - lag; i += 4) sum += norm[i] * norm[i+lag];
        if (sum > max_acf) { max_acf = sum; best_lag = lag; }
    }
    return 60.0 / (best_lag * sample_period);
}

pair<double, double> calculate_grid_score(const vector<double>& onsets, double bpm) {
    if (onsets.empty()) return {0,0};
    double period = 60000.0 / bpm;
    double max_hits = 0;
    double best_offset = 0;
    double tolerance = 15.0; 
    // brute forcing 20 phases shifts. primitive but passes the vibe check.
    // tried using math to calculate this directly but the results were inconsistent.
    for (int i = 0; i < 20; i++) {
        double test_offset = (period * i) / 20.0;
        double hits = 0;
        for (double t : onsets) {
            double diff = t - test_offset;
            while(diff < 0) diff += period;
            double rem = fmod(diff, period);
            // circular distance to nearest beat
            double dist = min(rem, period - rem);
            if (dist < tolerance) hits += 1.0;
        }
        if (hits > max_hits) { max_hits = hits; best_offset = test_offset; }
    }
    return {max_hits, best_offset};
}

// --- OFFSET LOGIC: ZERO-POINT PROJECTION ---
double calculate_offset_zero_point(const vector<double>& onsets, double bpm) {
    if (onsets.empty()) return 0;
    double period = 60000.0 / bpm;
    
    // 1. Phase Histogram
    // instead of trusting a single hit, i dump every hit's phase into bins.
    // the tallest bin is statistically where the "beat" lands. big brain moment.
    const int bins = 60;
    vector<int> histogram(bins, 0);
    
    for (double t : onsets) {
        double phase = fmod(t, period);
        int bin_idx = (int)((phase / period) * bins);
        if (bin_idx >= 0 && bin_idx < bins) histogram[bin_idx]++;
    }
    
    int max_bin = 0;
    int max_val = -1;
    // sliding window smoothing because raw bins are jagged af
    for (int i = 0; i < bins; i++) {
        int prev = (i - 1 + bins) % bins;
        int next = (i + 1) % bins;
        int sum = histogram[prev] + histogram[i] + histogram[next];
        if (sum > max_val) { max_val = sum; max_bin = i; }
    }
    
    // 2. Raw Offset
    double raw_offset = (max_bin / (double)bins) * period;
    
    // 3. Apply Latency
    raw_offset += SYSTEM_LATENCY_MS;
    
    // 4. Normalize to [0, Period)
    // this projects the offset backwards to the very start (t=0).
    // so we don't get an offset of 5000ms. negative offsets are cursed, avoid them.
    while(raw_offset < 0) raw_offset += period;
    while(raw_offset >= period) raw_offset -= period;

    return raw_offset;
}

struct AnalysisResult {
    double bpm;
    double offset;
    bool success;
};

AnalysisResult analyze_audio(const wstring& file_path, double forced_bpm = 0.0) {
    vector<double> onsets = get_onsets(file_path);
    
    double final_bpm = forced_bpm;

    if (final_bpm <= 0.0) {
        double sample_period;
        vector<double> flux = get_flux(file_path, sample_period);
        if (flux.empty()) return {0,0,false};
        double scout_bpm = scout_bpm_range(flux, sample_period);

        if (onsets.size() < 10) return {scout_bpm, 0, true};

        // --- BPM HEURISTICS ---
        // i'm generating candidates to fix the classic double/half bpm issue.
        // logic is kinda messy but covers most edge cases for dnb/hardcore/pop.
        vector<int> int_candidates;
        int center = (int)round(scout_bpm);
        for (int b = center - 10; b <= center + 10; ++b) int_candidates.push_back(b);
        if (center < 105) for (int b = center*2 - 5; b <= center*2 + 5; ++b) int_candidates.push_back(b);
        if (center > 210) for (int b = center/2 - 5; b <= center/2 + 5; ++b) int_candidates.push_back(b);
        // arbitrary biases because these bpms are common in rhythm games lol
        if (center > 220 && center < 230) int_candidates.push_back(180);
        if (center > 130 && center < 140) int_candidates.push_back(180);
        if (center > 260 && center < 280) int_candidates.push_back(180);
        
        sort(int_candidates.begin(), int_candidates.end());
        int_candidates.erase(unique(int_candidates.begin(), int_candidates.end()), int_candidates.end());

        double best_int_bpm = 0;
        double best_int_score = -1;

        for (int bpm : int_candidates) {
            if (bpm < 60 || bpm > 320) continue;
            pair<double, double> res = calculate_grid_score(onsets, (double)bpm);
            
            // weighting system (bias)
            // slightly rigging the election for 170-195bpm ranges.
            // also nerfing extremely fast stuff so it doesn't accidentally pick streams as 300bpm.
            double bias = 1.0;
            if (bpm >= 170 && bpm <= 195) bias = 1.05;
            if (bpm >= 120 && bpm < 170) bias = 1.05;
            if (bpm > 210) bias = 0.90; 
            if (bpm < 100) bias = 0.80;
            
            double weighted_score = res.first * bias;
            if (weighted_score > best_int_score) {
                best_int_score = weighted_score;
                best_int_bpm = (double)bpm;
            }
        }

        // final micro-optimization loop for floating point bpms.
        final_bpm = best_int_bpm;
        for (double b = best_int_bpm - 1.0; b <= best_int_bpm + 1.0; b += 0.01) {
            if (b == best_int_bpm) continue;
            pair<double, double> res = calculate_grid_score(onsets, b);
            
            // look, i know 1.05 (5%) is a huge threshold.
            // i tried lowering this to catch weird floats (like 165.7) but it made the detection for normal songs super unstable.
            // basically, if the float bpm isn't SIGNIFICANTLY better, stick to the integer.
            // i'm too lazy to write a better filter so this bias stays.
            if (res.first > best_int_score * 1.05) { 
                final_bpm = b;
            }
        }
        // snap to grid if it's close enough. again, avoiding 120.0001 bpm.
        if (abs(final_bpm - round(final_bpm)) < 0.05) final_bpm = round(final_bpm);
    }
    
    // Calculate offset based on final_bpm (whether manual or auto)
    // If onsets are empty (bad file), we can't calc offset
    if (onsets.empty()) return {final_bpm, 0, false};

    double final_offset = calculate_offset_zero_point(onsets, final_bpm);

    return {final_bpm, final_offset, true};
}

vector<wstring> open_file_dialog() {
    vector<wstring> files;
    const int buffer_size = 65536; 
    wchar_t* buffer = new wchar_t[buffer_size]; 
    buffer[0] = 0;
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = buffer_size;
    ofn.lpstrFilter = L"Audio Files\0*.mp3;*.ogg;*.wav\0\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
        wchar_t* p = ofn.lpstrFile;
        wstring directory = p;
        p += directory.length() + 1;
        if (*p == 0) files.push_back(directory);
        else {
            while (*p) {
                files.push_back(directory + L"\\" + p);
                p += wcslen(p) + 1;
            }
        }
    }
    delete[] buffer;
    return files;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    double manual_bpm = 0.0;
    string input_line;
    
    cout << "Enter BPM (leave blank for auto detection): ";
    getline(cin, input_line);

    if (!input_line.empty()) {
        try {
            manual_bpm = stod(input_line);
            if (manual_bpm < 0) manual_bpm = 0;
        } catch (...) {
            cout << "Invalid input. Defaulting to Auto Detection." << endl;
            manual_bpm = 0.0;
        }
    }

    system("cls");

    auto files = open_file_dialog();
    if (files.empty()) return 0;

    wcout << L"[0timer v1.0] Processing..." << endl;

    for (const auto& path : files) {
        fs::path p(path);
        AnalysisResult res = analyze_audio(path, manual_bpm);
        wcout << p.filename().wstring() << L": BPM: ";
        if (res.success) {
            cout << fixed << setprecision(3) << res.bpm << " | Offset: " << setprecision(0) << res.offset << endl;
        } else {
            wcout << L"ERROR" << endl;
        }
    }
    cout << "\nPress ENTER to exit.";
    cin.get();
    return 0;
}