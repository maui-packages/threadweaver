#ifndef EXCEPTION_H
#define EXCEPTION_H

#include <stdexcept>

#include <QString>

#include <threadweaver_export.h>

namespace ThreadWeaver {

class THREADWEAVER_EXPORT Exception : public std::runtime_error
{
public:
    explicit Exception(const QString& message = QString());
    ~Exception() throw();
    QString message() const;

private:
    QString m_message;
};

class THREADWEAVER_EXPORT JobAborted : public Exception
{
public:
    explicit JobAborted(const QString& message = QString());
};

class THREADWEAVER_EXPORT JobFailed : public Exception
{
public:
    explicit JobFailed(const QString& message = QString());
};

}

#endif // EXCEPTION_H
