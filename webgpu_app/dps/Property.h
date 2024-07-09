#pragma once

#include <QObject>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dps {

// Enumeration for property types
enum class PropertyType { UINT32, F32VEC4, GROUP };

// Abstract base class for properties
class Property : public QObject, public std::enable_shared_from_this<Property> {
    Q_OBJECT

public:
    virtual ~Property() = default;

    virtual std::string to_string() const = 0;
    virtual PropertyType type() const = 0;

    std::string get_name() const;
    std::shared_ptr<Property> get_parent() const;

    void add_child(const std::shared_ptr<Property>& child);
    const std::vector<std::shared_ptr<Property>>& get_children() const;

signals:
    void valueChanged();

protected:
    explicit Property(const std::string& propertyName);

    std::string name;
    std::weak_ptr<Property> parent;
    std::vector<std::shared_ptr<Property>> children;
    mutable std::mutex mtx;
};

} // namespace dps
