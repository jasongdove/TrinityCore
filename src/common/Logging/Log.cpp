/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Log.h"
#include "AppenderConsole.h"
#include "AppenderFile.h"
#include "Config.h"
#include "Errors.h"
#include "LogMessage.h"
#include "LogOperation.h"
#include "Logger.h"
#include "Strand.h"
#include "StringConvert.h"
#include "Util.h"

Log::Log() : AppenderId(0), lowestLogLevel(LOG_LEVEL_FATAL), m_logsTimestamp('_' + GetTimestampStr()), _ioContext(nullptr), _strand(nullptr)
{
    RegisterAppender<AppenderConsole>();
    RegisterAppender<AppenderFile>();
}

Log::~Log()
{
    delete _strand;
    Close();
}

uint8 Log::NextAppenderId()
{
    return AppenderId++;
}

Appender* Log::GetAppenderByName(std::string_view name)
{
    auto it = appenders.begin();
    while (it != appenders.end() && it->second && it->second->getName() != name)
        ++it;

    return it == appenders.end() ? nullptr : it->second.get();
}

void Log::CreateAppenderFromConfigLine(std::string const& appenderName, std::string const& options)
{
    if (appenderName.empty())
        return;

    // Format = type, level, flags, optional1, optional2
    // if type = File. optional1 = file and option2 = mode
    // if type = Console. optional1 = Color

    std::vector<std::string_view> tokens = Trinity::Tokenize(options, ',', true);

    size_t const size = tokens.size();
    std::string name = appenderName.substr(9);

    if (size < 2)
    {
        fprintf(stderr, "Log::CreateAppenderFromConfig: Wrong configuration for appender %s. Config line: %s\n", name.c_str(), options.c_str());
        return;
    }

    AppenderFlags flags = APPENDER_FLAGS_NONE;
    AppenderType type = AppenderType(Trinity::StringTo<uint8>(tokens[0]).value_or(APPENDER_INVALID));
    LogLevel level = LogLevel(Trinity::StringTo<uint8>(tokens[1]).value_or(LOG_LEVEL_INVALID));

    auto factoryFunction = appenderFactory.find(type);
    if (factoryFunction == appenderFactory.end())
    {
        fprintf(stderr, "Log::CreateAppenderFromConfig: Unknown type '" STRING_VIEW_FMT "' for appender %s\n", STRING_VIEW_FMT_ARG(tokens[0]), name.c_str());
        return;
    }

    if (level > NUM_ENABLED_LOG_LEVELS)
    {
        fprintf(stderr, "Log::CreateAppenderFromConfig: Wrong Log Level '" STRING_VIEW_FMT "' for appender %s\n", STRING_VIEW_FMT_ARG(tokens[1]), name.c_str());
        return;
    }

    if (size > 2)
    {
        if (Optional<uint8> flagsVal = Trinity::StringTo<uint8>(tokens[2]))
            flags = AppenderFlags(*flagsVal);
        else
        {
            fprintf(stderr, "Log::CreateAppenderFromConfig: Unknown flags '" STRING_VIEW_FMT "' for appender %s\n", STRING_VIEW_FMT_ARG(tokens[2]), name.c_str());
            return;
        }
    }

    try
    {
        Appender* appender = factoryFunction->second(NextAppenderId(), std::move(name), level, flags, tokens);
        appenders[appender->getId()].reset(appender);
    }
    catch (InvalidAppenderArgsException const& iaae)
    {
        fprintf(stderr, "%s\n", iaae.what());
    }
}

void Log::CreateAppenderFromConfig(std::string const& appenderName)
{
    CreateAppenderFromConfigLine(appenderName, sConfigMgr->GetStringDefault(appenderName, ""));
}

void Log::CreateLoggerFromConfigLine(std::string const& loggerName, std::string const& options)
{
    if (loggerName.empty())
        return;

    LogLevel level = LOG_LEVEL_DISABLED;

    std::string name = loggerName.substr(7);

    if (options.empty())
    {
        fprintf(stderr, "Log::CreateLoggerFromConfig: Missing config option Logger.%s\n", name.c_str());
        return;
    }

    std::vector<std::string_view> tokens = Trinity::Tokenize(options, ',', true);

    if (tokens.size() != 2)
    {
        fprintf(stderr, "Log::CreateLoggerFromConfig: Wrong config option Logger.%s=%s\n", name.c_str(), options.c_str());
        return;
    }

    if (loggers.find(name) != loggers.end())
    {
        fprintf(stderr, "Error while configuring Logger %s. Already defined\n", name.c_str());
        return;
    }

    level = LogLevel(Trinity::StringTo<uint8>(tokens[0]).value_or(LOG_LEVEL_INVALID));
    if (level > NUM_ENABLED_LOG_LEVELS)
    {
        fprintf(stderr, "Log::CreateLoggerFromConfig: Wrong Log Level '" STRING_VIEW_FMT "' for logger %s\n", STRING_VIEW_FMT_ARG(tokens[0]), name.c_str());
        return;
    }

    if (level < lowestLogLevel)
        lowestLogLevel = level;

    Logger* logger = new Logger(name, level);
    loggers[logger->getName()].reset(logger);
    //fprintf(stdout, "Log::CreateLoggerFromConfig: Created Logger %s, Level %u\n", name.c_str(), level);

    for (std::string_view appenderName : Trinity::Tokenize(tokens[1], ' ', false))
    {
        if (Appender* appender = GetAppenderByName(appenderName))
        {
            logger->addAppender(appender);
            //fprintf(stdout, "Log::CreateLoggerFromConfig: Added Appender %s to Logger %s\n", appender->getName().c_str(), name.c_str());
        }
        else
            fprintf(stderr, "Error while configuring Appender " STRING_VIEW_FMT " in Logger %s. Appender does not exist\n", STRING_VIEW_FMT_ARG(appenderName), name.c_str());
    }
}

void Log::CreateLoggerFromConfig(std::string const& loggerName)
{
    CreateLoggerFromConfigLine(loggerName, sConfigMgr->GetStringDefault(loggerName, ""));
}

void Log::ReadAppendersFromConfig()
{
    std::vector<std::string> keys = sConfigMgr->GetKeysByString("Appender.");
    for (std::string const& appenderName : keys)
        CreateAppenderFromConfig(appenderName);
}

void Log::ReadLoggersFromConfig()
{
    std::vector<std::string> keys = sConfigMgr->GetKeysByString("Logger.");
    for (std::string const& loggerName : keys)
        CreateLoggerFromConfig(loggerName);

    // Bad config configuration, creating default config
    if (loggers.find(LOGGER_ROOT) == loggers.end())
    {
        fprintf(stderr, "Wrong Loggers configuration. Review your Logger config section.\n"
                        "Creating default loggers [root (Error), server (Info)] to console\n");

        Close(); // Clean any Logger or Appender created

        AppenderConsole* appender = new AppenderConsole(NextAppenderId(), "Console", LOG_LEVEL_DEBUG, APPENDER_FLAGS_NONE, {});
        appenders[appender->getId()].reset(appender);

        Logger* rootLogger = new Logger(LOGGER_ROOT, LOG_LEVEL_ERROR);
        rootLogger->addAppender(appender);
        loggers[rootLogger->getName()].reset(rootLogger);

        Logger* serverLogger = new Logger("server", LOG_LEVEL_INFO);
        serverLogger->addAppender(appender);
        loggers[serverLogger->getName()].reset(serverLogger);
    }
}

void Log::RegisterAppender(uint8 index, AppenderCreatorFn appenderCreateFn)
{
    [[maybe_unused]] bool isNewAppender = appenderFactory.try_emplace(index, appenderCreateFn).second;
    ASSERT(isNewAppender);
}

void Log::OutMessageImpl(Logger const* logger, std::string_view filter, LogLevel level, Trinity::FormatStringView messageFormat, Trinity::FormatArgs messageFormatArgs) const noexcept
{
    if (_ioContext)
        Trinity::Asio::post(*_strand, LogOperation(logger, new LogMessage(level, filter, Trinity::StringVFormat(messageFormat, messageFormatArgs))));
    else
    {
        LogMessage msg(level, filter, Trinity::StringVFormat(messageFormat, messageFormatArgs));
        logger->write(&msg);
    }
}

void Log::OutCommandImpl(uint32 account, Trinity::FormatStringView messageFormat, Trinity::FormatArgs messageFormatArgs) const noexcept
{
    Logger const* logger = GetLoggerByType("commands.gm");

    if (_ioContext)
        Trinity::Asio::post(*_strand, LogOperation(logger, new LogMessage(LOG_LEVEL_INFO, "commands.gm", Trinity::StringVFormat(messageFormat, messageFormatArgs), Trinity::ToString(account))));
    else
    {
        LogMessage msg(LOG_LEVEL_INFO, "commands.gm", Trinity::StringVFormat(messageFormat, messageFormatArgs), Trinity::ToString(account));
        logger->write(&msg);
    }
}

Logger const* Log::GetLoggerByType(std::string_view type) const
{
    auto it = loggers.find(type);
    if (it != loggers.end())
        return it->second.get();

    if (type == LOGGER_ROOT)
        return nullptr;

    std::string_view parentLogger = LOGGER_ROOT;
    size_t found = type.find_last_of('.');
    if (found != std::string::npos)
        parentLogger = type.substr(0, found);

    return GetLoggerByType(parentLogger);
}

std::string Log::GetTimestampStr()
{
    return TimeToTimestampStr(time(nullptr));
}

bool Log::SetLogLevel(std::string const& name, int32 newLeveli, bool isLogger /* = true */)
{
    if (newLeveli < 0)
        return false;

    LogLevel newLevel = LogLevel(newLeveli);

    if (isLogger)
    {
        auto it = loggers.begin();
        while (it != loggers.end() && it->second->getName() != name)
            ++it;

        if (it == loggers.end())
            return false;

        it->second->setLogLevel(newLevel);

        if (newLevel != LOG_LEVEL_DISABLED && newLevel < lowestLogLevel)
            lowestLogLevel = newLevel;
    }
    else
    {
        Appender* appender = GetAppenderByName(name);
        if (!appender)
            return false;

        appender->setLogLevel(newLevel);
    }

    return true;
}

void Log::OutCharDump(std::string const& str, uint32 accountId, uint64 guid, std::string const& name) const noexcept
{
    if (!ShouldLog("entities.player.dump", LOG_LEVEL_INFO))
        return;

    std::string ss = Trinity::StringFormat("== START DUMP == (account: {} guid: {} name: {})\n{}\n== END DUMP ==\n", accountId, guid, name, str);
    std::string param = Trinity::StringFormat("{}_{}", guid, name);

    Logger const* logger = GetLoggerByType("entities.player.dump");

    if (_ioContext)
        Trinity::Asio::post(*_strand, LogOperation(logger, new LogMessage(LOG_LEVEL_INFO, "entities.player.dump", std::move(ss), std::move(param))));
    else
    {
        LogMessage msg(LOG_LEVEL_INFO, "entities.player.dump", std::move(ss), std::move(param));
        logger->write(&msg);
    }
}

void Log::SetRealmId(uint32 id)
{
    for (std::pair<uint8 const, std::unique_ptr<Appender>>& appender : appenders)
        appender.second->setRealmId(id);
}

void Log::Close()
{
    loggers.clear();
    appenders.clear();
}

bool Log::ShouldLog(std::string_view type, LogLevel level) const noexcept
{
    // TODO: Use cache to store "Type.sub1.sub2": "Type" equivalence, should
    // Speed up in cases where requesting "Type.sub1.sub2" but only configured
    // Logger "Type"

    // Don't even look for a logger if the LogLevel is lower than lowest log levels across all loggers
    if (level < lowestLogLevel)
        return false;

    Logger const* logger = GetLoggerByType(type);
    if (!logger)
        return false;

    LogLevel logLevel = logger->getLogLevel();
    return logLevel != LOG_LEVEL_DISABLED && logLevel <= level;
}

Logger const* Log::GetEnabledLogger(std::string_view type, LogLevel level) const noexcept
{
    // Don't even look for a logger if the LogLevel is lower than lowest log levels across all loggers
    if (level < lowestLogLevel)
        return nullptr;

    Logger const* logger = GetLoggerByType(type);
    if (!logger)
        return nullptr;

    LogLevel logLevel = logger->getLogLevel();
    return logLevel != LOG_LEVEL_DISABLED && logLevel <= level ? logger : nullptr;
}

Log* Log::instance() noexcept
{
    static Log instance;
    return &instance;
}

void Log::Initialize(Trinity::Asio::IoContext* ioContext)
{
    if (ioContext)
    {
        _ioContext = ioContext;
        _strand = new Trinity::Asio::Strand(*ioContext);
    }

    LoadFromConfig();
}

void Log::SetSynchronous()
{
    delete _strand;
    _strand = nullptr;
    _ioContext = nullptr;
}

void Log::LoadFromConfig()
{
    Close();

    lowestLogLevel = LOG_LEVEL_FATAL;
    AppenderId = 0;
    m_logsDir = sConfigMgr->GetStringDefault("LogsDir", "");
    if (!m_logsDir.empty())
        if ((m_logsDir.at(m_logsDir.length() - 1) != '/') && (m_logsDir.at(m_logsDir.length() - 1) != '\\'))
            m_logsDir.push_back('/');

    ReadAppendersFromConfig();
    ReadLoggersFromConfig();
}
