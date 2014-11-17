#include <fstream>
#include "kernel.hpp"
#include <boost/property_tree/xml_parser.hpp>
#include <tools/log.hpp>
#include <tools/signalhandler.hpp>
#include <tools/unixshellscript.hpp>
#include "exception/configexception.hpp"
#include "Logger.hpp"
#include "tools/XmlTreeWriter.hpp"
#include <boost/property_tree/ptree_serialization.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/binary_iarchive.hpp>

using boost::property_tree::xml_parser::read_xml;
using boost::property_tree::xml_parser::write_xml;
using boost::property_tree::xml_writer_settings;
using boost::property_tree::ptree_error;
using boost::property_tree::xml_parser::no_concat_text;
using boost::property_tree::xml_parser::no_comments;
using boost::property_tree::xml_parser::trim_whitespace;
using boost::property_tree::ptree;

// move somewhere else??
thread_local zmqpp::socket *tl_log_socket = nullptr;

Kernel::Kernel(const boost::property_tree::ptree &config) :
        ctx_(),
        log_socket_guard_(ctx_),
        logger_(std::bind(&Kernel::run_logger, this, std::placeholders::_1)),
        bus_(ctx_),
        control_(ctx_, zmqpp::socket_type::rep),
        config_(config),
        is_running_(true),
        want_restart_(false),
        module_manager_(ctx_),
        network_config_(config.get_child("network"))
{
    control_.bind("inproc://leosac-kernel");
    network_config_.reload();
}

boost::property_tree::ptree Kernel::make_config(const RuntimeOptions &opt)
{
    boost::property_tree::ptree cfg;
    std::string filename(opt.getParam("kernel-cfg"));
    std::ifstream cfg_stream(filename);

    if (!cfg_stream.good())
        throw (ConfigException(filename, "Could not open file"));
    try
    {
        read_xml(cfg_stream, cfg, trim_whitespace | no_comments);
        // store the path the config file.
        cfg.get_child("kernel").add("kernel-cfg", filename);
        return cfg.get_child("kernel"); // kernel is the root node.
    }
    catch (ptree_error &e)
    {
        throw (ConfigException(filename, std::string("Boost exception: ") + e.what()));
    }
}

bool Kernel::run()
{
    if (!module_manager_init())
        return false;

    SignalHandler::registerCallback(Signal::SigInt, [this](Signal)
    {
        this->is_running_ = false;
    });

    SignalHandler::registerCallback(Signal::SigTerm, [this](Signal)
    {
        this->is_running_ = false;
    });

    reactor_.add(control_, std::bind(&Kernel::handle_control_request, this));
    while (is_running_)
    {
        reactor_.poll(-1);
    }

    return want_restart_;
}

bool Kernel::module_manager_init()
{
    try
    {
        ptree plugin_dirs = config_.get_child("plugin_directories");

        for (const auto &plugin_dir : plugin_dirs)
        {
            std::string pname = plugin_dir.first;
            std::string pvalue = plugin_dir.second.data();

            assert(pname == "plugindir");
            DEBUG("Adding {" << pvalue << "} in library path");
            module_manager_.addToPath(pvalue);
        }

        for (const auto &module : config_.get_child("modules"))
        {
            std::string pname = module.first;
            assert(pname == "module");

            ptree module_conf = module.second;
            std::string module_file = module_conf.get_child("file").data();

            if (!module_manager_.loadModule(module_conf))
                return false;
        }
    }
    catch (ptree_error &e)
    {
        ERROR("Invalid configuration file: " << e.what());
        return false;
    }
    if (!module_manager_.initModules())
    {
        return false;
    }
    return true;
}

void Kernel::handle_control_request()
{
    zmqpp::message msg;
    std::string req;

    control_.receive(msg);
    msg >> req;
    INFO("Receive request: " << req);

    if (req == "RESTART")
    {
        is_running_ = false;
        want_restart_ = true;
        control_.send("OK");
    }
    if (req == "RESET")
    {
        is_running_ = false;
        want_restart_ = true;
        factory_reset();
    }
    if (req == "GET_NETCONFIG")
    {
        get_netconfig();
    }
    if (req == "SET_NETCONFIG")
    {
        set_netconfig(&msg);
    }
}

void Kernel::factory_reset()
{
    // we need to restore factory config file.
    UnixShellScript script("cp -f");

    std::string kernel_config_file = config_.get_child("kernel-cfg").data();
    INFO("Kernel config file path = " << kernel_config_file);
    INFO("RESTORING FACTORY CONFIG");

    if (script.run(UnixShellScript::toCmdLine(std::string(rel_path_to_factory_conf_) + "/kernel.xml",
            kernel_config_file)) != 0)
    {
        ERROR("Error restoring factory configuration...");
    }
}

bool Kernel::run_logger(zmqpp::socket *pipe)
{
    boost::property_tree::ptree cfg;

    Leosac::Logger::LoggerSink module(ctx_, pipe, cfg);
    pipe->send(zmqpp::signal::ok);

    module.run();
    std::cout << "logger down" << std::endl;
    return true;
}

Kernel::LogSocketGuard::~LogSocketGuard()
{
    delete tl_log_socket;
    tl_log_socket = nullptr;
}

void Kernel::get_netconfig()
{
    std::ostringstream oss;
    boost::archive::binary_oarchive archive(oss);
    auto network_config = config_.get_child("network");

    zmqpp::message response;
    boost::property_tree::save(archive, network_config, 1);
    response << oss.str();
    control_.send(response);
}

void Kernel::set_netconfig(zmqpp::message *msg)
{
    std::string serialized_config;
    *msg >> serialized_config;
    std::istringstream iss(serialized_config);
    boost::archive::binary_iarchive archive(iss);

    boost::property_tree::ptree network_config;
    boost::property_tree::load(archive, network_config, 1);

    config_.erase("network");
    config_.add_child("network", network_config);

    // we need to add the root node to write config;
    boost::property_tree::ptree to_save;

    to_save.add_child("kernel", config_);
    // remove path to config file.
    to_save.get_child("kernel").erase("kernel-cfg");
    try
    {
        Leosac::Tools::propertyTreeToXmlFile(to_save, config_.get_child("kernel-cfg").data());
    }
    catch (std::exception &e)
    {
        ERROR("Exception: " << e.what());
        control_.send("KO");
        return;
    }
    control_.send("OK");
}

Kernel::LogSocketGuard::LogSocketGuard(zmqpp::context &ctx)
{
    //init log socket for main thread
    tl_log_socket = new zmqpp::socket(ctx, zmqpp::socket_type::push);
    tl_log_socket->connect("inproc://log-sink");
}
