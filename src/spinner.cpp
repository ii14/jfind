#include "../include/spinner.hpp"

using namespace std::chrono_literals;
using std::chrono::milliseconds;
using std::chrono::system_clock;

const char *SPINNER[6] = {"⠇", "⠋", "⠙", "⠸", "⠴", "⠦"};
const int SPINNER_SIZE = 6;

Spinner::Spinner(FILE *file) {
    m_outputFile = file;
}

milliseconds Spinner::frameTimeRemaining() {
    milliseconds remaining = 150ms - duration_cast<milliseconds>(
            system_clock::now() - m_lastFrameTime);
    if (remaining <= 0ms) {
        return 0ms;
    }
    return remaining;
}

void Spinner::setPosition(int x, int y) {
    m_x = x;
    m_y = y;
}

void Spinner::update() {
    if (m_firstUpdate) {
        m_firstUpdate = false;
        return;
    }
    m_firstUpdateComplete = true;
    draw();
    m_frame = (m_frame + 1) % SPINNER_SIZE;
    m_lastFrameTime = system_clock::now();
}

void Spinner::draw() {
    if (m_firstUpdateComplete) {
        ansi.move(m_x, m_y);
        fprintf(m_outputFile, "%s", SPINNER[m_frame]);
    }
}

bool Spinner::isSpinning() {
    return m_isSpinning;
}

void Spinner::setSpinning(bool value) {
    if (!m_isSpinning) {
        m_lastFrameTime = system_clock::now();
        m_firstUpdate = true;
        m_firstUpdateComplete = false;
    }
    m_isSpinning = value;
}
