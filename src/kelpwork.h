#ifndef KELPWORK_H
#define KELPWORK_H

#include <QThread>

#include <functional>

/*
 * One short-lived worker thread for work that must not hold the UI.
 *
 * Sealing a folder and deriving a key from a passphrase are both CPU-bound and
 * both used to run on the GUI thread: the first can take minutes for a large
 * folder, the second is 600 000 rounds of PBKDF2 by design. Neither needs the
 * event loop, so both run here and report back on `finished()`.
 *
 * The work must not touch the object that owns this thread. It is handed copies
 * of what it needs and leaves its answer in `result()`, which the owner reads
 * after `finished()` arrives on the owner's thread. The destructor waits, so a
 * thread outliving its owner is impossible.
 */
template <typename Result>
class KelpWork : public QThread
{
public:
    typedef std::function<Result()> Work;

    explicit KelpWork(Work work, QObject *parent = nullptr)
        : QThread(parent), m_work(work)
    {
    }

    ~KelpWork() override { wait(); }

    Result result() const { return m_result; }

protected:
    void run() override { m_result = m_work(); }

private:
    Work m_work;
    Result m_result;
};

#endif // KELPWORK_H
