#include <iostream>
#include <fstream>
#include <string>
#include <regex>
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <future>
#include <mutex>
#include "json.hpp"
 
using json = nlohmann::json;

// ------------------------------------------------------------
// Класс Config: загрузка и хранение конфигурации
// ------------------------------------------------------------
class Config
{
public:
    struct Sensor
    {
        std::string name;
        std::string rule; // строка поиска начала датчика (например "Датчик 1")
    };

    struct Rule
    {
        std::string name;
        std::string type; // "bool", "value", "speed"
        std::regex regex;
        std::string true_str;
        std::string false_str;
    };

    Config(const std::string &filename)
    {
        std::ifstream file(filename);
        if (!file.is_open())
            throw std::runtime_error("Не удалось открыть config.json");
        json j;
        file >> j;

        // Загружаем сенсоры
        for (const auto &item : j["sensors"])
        {
            sensors_.push_back({item["name"], item["rule"]});
        }

        // Загружаем правила
        for (const auto &item : j["rules"])
        {
            Rule r;
            r.name = item["name"];
            r.type = item["type"];
            r.regex = std::regex(item["rule"]);
            if (r.type == "bool")
            {
                r.true_str = item["true"];
                r.false_str = item["false"];
            }
            rules_[r.name] = r;
        }

        // Загружаем экстракторы (связи сенсор -> список правил)
        for (const auto &item : j["extractors"])
        {
            std::string sensor_name = item["sensor"];
            std::vector<std::string> rule_list;
            for (const auto &r : item["rules"])
            {
                rule_list.push_back(r);
            }
            sensor_to_rules_[sensor_name] = rule_list;
        }
    }

    const std::vector<Sensor> &sensors() const { return sensors_; }
    const std::map<std::string, Rule> &rules() const { return rules_; }
    const std::map<std::string, std::vector<std::string>> &sensorToRules() const { return sensor_to_rules_; }

private:
    std::vector<Sensor> sensors_;
    std::map<std::string, Rule> rules_;
    std::map<std::string, std::vector<std::string>> sensor_to_rules_;
};

// ------------------------------------------------------------
// Класс ValueParser: преобразование строки в числовое значение для сравнения
// ------------------------------------------------------------
class ValueParser
{
public:
    struct ParsedValue
    {
        double numeric;         // для сравнения (bool->0/1, число, бит/с)
        std::string raw_string; // исходная строка для вывода
        bool valid;
    };

    static ParsedValue parse(const std::string &matched_str, const Config::Rule &rule)
    {
        ParsedValue result;
        result.valid = true;
        result.raw_string = matched_str;

        try
        {
            if (rule.type == "bool")
            {
                if (matched_str == rule.true_str)
                    result.numeric = 1.0;
                else if (matched_str == rule.false_str)
                    result.numeric = 0.0;
                else
                {
                    result.valid = false;
                    std::cerr << "Ошибка: неизвестное значение bool '" << matched_str << "'\n";
                }
            }
            else if (rule.type == "value")
            {
                result.numeric = std::stod(matched_str);
            }
            else if (rule.type == "speed")
            {
                result.numeric = speedToBps(matched_str);
            }
            else
            {
                result.valid = false;
                std::cerr << "Ошибка: неизвестный тип правила " << rule.type << "\n";
            }
        }
        catch (const std::exception &e)
        {
            result.valid = false;
            std::cerr << "Ошибка парсинга значения '" << matched_str << "': " << e.what() << "\n";
        }
        return result;
    }

private:
    static double speedToBps(const std::string &speed_str)
    {
        std::regex re(R"((\d+(?:\.\d+)?)\s*([KMG]?bit/s))");
        std::smatch m;
        if (std::regex_match(speed_str, m, re))
        {
            double val = std::stod(m[1]);
            std::string unit = m[2];
            if (unit == "bit/s")
                return val;
            if (unit == "Kbit/s")
                return val * 1000.0;
            if (unit == "Mbit/s")
                return val * 1'000'000.0;
            if (unit == "Gbit/s")
                return val * 1'000'000'000.0;
        }
        throw std::runtime_error("Неверный формат скорости: " + speed_str);
    }
};

// ------------------------------------------------------------
// Класс Statistics: хранит глобальные мин/макс для (сенсор, правило)
// ------------------------------------------------------------
class Statistics
{
public:
    struct Entry
    {
        double min_val;
        double max_val;
        std::string min_file;
        std::string max_file;
        std::string min_raw;
        std::string max_raw;
    };

    void update(const std::string &sensor,
                const std::string &rule_name,
                const ValueParser::ParsedValue &value,
                const std::string &filename)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = std::make_pair(sensor, rule_name);
        auto &entry = stats_[key];
        if (entry.min_file.empty())
        {
            entry.min_val = entry.max_val = value.numeric;
            entry.min_file = entry.max_file = filename;
            entry.min_raw = entry.max_raw = value.raw_string;
        }
        else
        {
            if (value.numeric < entry.min_val)
            {
                entry.min_val = value.numeric;
                entry.min_file = filename;
                entry.min_raw = value.raw_string;
            }
            if (value.numeric > entry.max_val)
            {
                entry.max_val = value.numeric;
                entry.max_file = filename;
                entry.max_raw = value.raw_string;
            }
        }
    }

    const Entry *get(const std::string &sensor, const std::string &rule_name) const
    {
        auto it = stats_.find(std::make_pair(sensor, rule_name));
        return (it != stats_.end()) ? &it->second : nullptr;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::pair<std::string, std::string>, Entry> stats_;
};

// ------------------------------------------------------------
// Класс FileProcessor: обрабатывает один файл, обновляет статистику
// ------------------------------------------------------------
class FileProcessor
{
public:
    FileProcessor(const Config &config, Statistics &stats)
        : config_(config), stats_(stats) {}

    void process(const std::string &filename) const
    {
        std::ifstream file(filename);
        if (!file.is_open())
        {
            std::cerr << "Не удалось открыть файл: " << filename << "\n";
            return;
        }

        std::string line;
        std::string current_sensor;
        std::map<std::string, bool> rule_found;

        auto finalize_sensor = [&]()
        {
            if (!current_sensor.empty())
            {
                const auto &rules_for_sensor = config_.sensorToRules().at(current_sensor);
                for (const auto &rname : rules_for_sensor)
                {
                    if (!rule_found[rname])
                    {
                        std::cerr << "В файле " << filename << " для датчика " << current_sensor
                                  << " не найдено правило: " << rname << "\n";
                    }
                }
                current_sensor.clear();
                rule_found.clear();
            }
        };

        while (std::getline(file, line))
        {
            // Поиск начала нового датчика
            bool found_new_sensor = false;
            for (const auto &sens : config_.sensors())
            {
                if (line.find(sens.rule) != std::string::npos)
                {
                    finalize_sensor();
                    current_sensor = sens.name;
                    // Инициализируем rule_found для этого сенсора
                    const auto &rnames = config_.sensorToRules().at(current_sensor);
                    for (const auto &rn : rnames)
                        rule_found[rn] = false;
                    found_new_sensor = true;
                    break;
                }
            }
            if (found_new_sensor)
                continue;
            if (current_sensor.empty())
                continue;

            // Обрабатываем строку текущего датчика
            const auto &rule_names = config_.sensorToRules().at(current_sensor);
            for (const auto &rname : rule_names)
            {
                if (rule_found[rname])
                    continue;
                const Config::Rule &rule = config_.rules().at(rname);
                std::smatch match;
                if (std::regex_search(line, match, rule.regex))
                {
                    std::string matched_str = match[1].str();
                    // Для скорости берём две группы
                    if (rule.type == "speed" && match.size() >= 3)
                    {
                        matched_str = match[1].str() + " " + match[2].str() + "/s";
                    }
                    auto parsed = ValueParser::parse(matched_str, rule);
                    if (parsed.valid)
                    {
                        rule_found[rname] = true;
                        stats_.update(current_sensor, rname, parsed, filename);
                    }
                }
            }
        }
        finalize_sensor();
    }

private:
    const Config &config_;
    Statistics &stats_;
};

// ------------------------------------------------------------
// Главный класс SensorParser: управляет всей программой
// ------------------------------------------------------------
class SensorParser
{
public:
    SensorParser(const Config &config) : config_(config) {}

    void run(int argc, char *argv[])
    {
        if (argc < 2)
        {
            std::cerr << "Использование: " << argv[0] << " file1.txt file2.txt ...\n";
            return;
        }

        // Параллельная обработка файлов
        std::vector<std::future<void>> futures;
        for (int i = 1; i < argc; ++i)
        {
            // Создаём копию FileProcessor для каждого потока (разделяемые config и stats)
            // stats_ разделяется между потоками, но внутри Statistics есть мьютекс
            futures.push_back(std::async(std::launch::async, [this, fname = std::string(argv[i])]()
                                         {
                FileProcessor processor(config_, stats_);
                processor.process(fname); }));
        }
        // Ждём завершения всех задач
        for (auto &fut : futures)
        {
            fut.get();
        }

        // Вывод результатов
        for (const auto &sens : config_.sensors())
        {
            std::cout << sens.name << ":\n";
            const auto &rule_names = config_.sensorToRules().at(sens.name);
            for (const auto &rname : rule_names)
            {
                const auto *entry = stats_.get(sens.name, rname);
                if (!entry)
                {
                    std::cerr << "Нет данных для " << sens.name << " / " << rname << "\n";
                    continue;
                }
                std::cout << "    " << rname << ": max=" << entry->max_raw << "(" << entry->max_file
                          << "), min=" << entry->min_raw << "(" << entry->min_file << ")\n";
            }
        }
    }

private:
    const Config &config_;
    Statistics stats_;
};

// ------------------------------------------------------------
// Точка входа
// ------------------------------------------------------------
int main(int argc, char *argv[])
{
    try
    {
        Config config("config.json");
        SensorParser parser(config);
        parser.run(argc, argv);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Ошибка: " << e.what() << "\n";
        return 1;
    }
    return 0;
}