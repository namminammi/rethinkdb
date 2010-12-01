#include "server.hpp"
#include "db_cpu_info.hpp"

server_t::server_t(cmd_config_t *cmd_config, thread_pool_t *thread_pool)
    : cmd_config(cmd_config), thread_pool(thread_pool),
      conn_acceptor(this),
      toggler(this) { }

void server_t::do_start() {
    
    assert_cpu();
    do_start_loggers();
}

void server_t::do_start_loggers() {
    logger_start(this);
}

void server_t::on_logger_ready() {
    if (cmd_config->create_store && !cmd_config->force_create) do_check_store();
    else do_start_store();
}

void server_t::do_check_store() {
    /* Copy database filenames from private serializer configurations into a single vector of strings */
    std::vector<std::string> db_filenames;
    std::vector<log_serializer_private_dynamic_config_t>& serializer_private = cmd_config->store_dynamic_config.serializer_private;
    std::vector<log_serializer_private_dynamic_config_t>::iterator it;

    for (it = serializer_private.begin(); it != serializer_private.end(); ++it) {
        db_filenames.push_back((*it).db_filename);
    }
    btree_key_value_store_t::check_existing(db_filenames, this);
}

void server_t::on_store_check(bool ok) {
    if (ok) {
        fail("It looks like there already is a database here. RethinkDB will abort in case you "
            "didn't mean to overwrite it. Run with the '--force' flag to override this warning.");
    }
    do_start_store();
}

void server_t::do_start_store() {

    assert_cpu();
    
    store = new btree_key_value_store_t(&cmd_config->store_dynamic_config);
    
    bool done;
    if (cmd_config->create_store) {
        logINF("Creating new database...\n");
        done = store->start_new(this, &cmd_config->store_static_config);
    } else {
        logINF("Loading existing database...\n");
        done = store->start_existing(this);
    }
    if (done) on_store_ready();
}

void server_t::on_store_ready() {

    assert_cpu();
    
    if (cmd_config->shutdown_after_creation) {
        logINF("Done creating database.\n");
        do_shutdown_store();
    } else {
        do_start_conn_acceptor();
    }
}

void server_t::do_start_conn_acceptor() {
    
    assert_cpu();

    // We've now loaded everything. It's safe to print the config
    // specs (part of them depends on the information loaded from the
    // db file).
    print_config(cmd_config);
    
    logINF("Server is now accepting memcached connections on port %d.\n", cmd_config->port);
    
    conn_acceptor.start();
    
    interrupt_message.server = this;
    thread_pool->set_interrupt_message(&interrupt_message);
}

void server_t::shutdown() {
    
    /* This can be called from any CPU! */
    
    cpu_message_t *old_interrupt_msg = thread_pool->set_interrupt_message(NULL);
    
    /* If the interrupt message already was NULL, that means that either shutdown() was for
    some reason called before we finished starting up or shutdown() was called twice and this
    is the second time. */
    if (!old_interrupt_msg) return;
    
    if (continue_on_cpu(home_cpu, old_interrupt_msg))
        call_later_on_this_cpu(old_interrupt_msg);
}

void server_t::do_shutdown() {
    
    logINF("Shutting down...\n");
    
    assert_cpu();
    do_shutdown_conn_acceptor();
}

void server_t::do_shutdown_conn_acceptor() {
    if (conn_acceptor.shutdown(this)) on_conn_acceptor_shutdown();
}

void server_t::on_conn_acceptor_shutdown() {
    assert_cpu();
    do_shutdown_store();
}

void server_t::do_shutdown_store() {
    if (store->shutdown(this)) on_store_shutdown();
}

void server_t::on_store_shutdown() {
    assert_cpu();
    delete store;
    store = NULL;
    do_shutdown_loggers();
}

void server_t::do_shutdown_loggers() {
    logger_shutdown(this);
}

void server_t::on_logger_shutdown() {
    assert_cpu();
    do_message_flush();
}

/* The penultimate step of shutting down is to make sure that all messages
have reached their destinations so that they can be freed. The way we do this
is to send one final message to each core; when those messages all get back
we know that all messages have been processed properly. Otherwise, logger
shutdown messages would get "stuck" in the message hub when it shut down,
leading to memory leaks. */

struct flush_message_t :
    public cpu_message_t
{
    bool returning;
    server_t *server;
    void on_cpu_switch() {
        if (returning) {
            server->on_message_flush();
            delete this;
        } else {
            returning = true;
            if (continue_on_cpu(server->home_cpu, this)) on_cpu_switch();
        }
    }
};

void server_t::do_message_flush() {
    messages_out = get_num_cpus();
    for (int i = 0; i < get_num_cpus(); i++) {
        flush_message_t *m = new flush_message_t();
        m->returning = false;
        m->server = this;
        if (continue_on_cpu(i, m)) m->on_cpu_switch();
    }
}

void server_t::on_message_flush() {
    messages_out--;
    if (messages_out == 0) {
        do_stop_threads();
    }
}

void server_t::do_stop_threads() {
    
    assert_cpu();
    // This returns immediately, but will cause all of the threads to stop after we
    // return to the event queue.
    thread_pool->shutdown();
    delete this;
}

bool server_t::disable_gc(all_gc_disabled_callback_t *cb) {
    // The callback always gets called.

    return do_on_cpu(home_cpu, this, &server_t::do_disable_gc, cb);
}

bool server_t::do_disable_gc(all_gc_disabled_callback_t *cb) {
    assert_cpu();

    return toggler.disable_gc(cb);
}

bool server_t::enable_gc(all_gc_enabled_callback_t *cb) {
    // The callback always gets called.

    return do_on_cpu(home_cpu, this, &server_t::do_enable_gc, cb);
}

bool server_t::do_enable_gc(all_gc_enabled_callback_t *cb) {
    assert_cpu();

    return toggler.enable_gc(cb);
}


server_t::gc_toggler_t::gc_toggler_t(server_t *server) : state_(enabled), num_disabled_serializers_(0), server_(server) { }

bool server_t::gc_toggler_t::disable_gc(server_t::all_gc_disabled_callback_t *cb) {
    // We _always_ call the callback.

    if (state_ == enabled) {
        assert(callbacks_.size() == 0);

        int num_serializers = server_->cmd_config->store_dynamic_config.serializer_private.size();
        assert(num_serializers > 0);

        state_ = disabling;

        callbacks_.push_back(cb);

        num_disabled_serializers_ = 0;
        for (int i = 0; i < num_serializers; ++i) {
            standard_serializer_t::gc_disable_callback_t *this_as_callback = this;
            do_on_cpu(server_->store->serializers[i]->home_cpu, server_->store->serializers[i], &standard_serializer_t::disable_gc, this_as_callback);
        }

        return (state_ == disabled);
    } else if (state_ == disabling) {
        assert(callbacks_.size() > 0);
        callbacks_.push_back(cb);
        return false;
    } else if (state_ == disabled) {
        assert(state_ == disabled);
        cb->multiple_users_seen = true;
        cb->on_gc_disabled();
        return true;
    } else {
        assert(0);
        return false;  // Make compiler happy.
    }
}

bool server_t::gc_toggler_t::enable_gc(all_gc_enabled_callback_t *cb) {
    // Always calls the callback.

    int num_serializers = server_->cmd_config->store_dynamic_config.serializer_private.size();

    // The return value of serializer_t::enable_gc is always true.

    for (int i = 0; i < num_serializers; ++i) {
        do_on_cpu(server_->store->serializers[i]->home_cpu, server_->store->serializers[i], &standard_serializer_t::enable_gc);
    }

    if (state_ == enabled) {
        cb->multiple_users_seen = true;
    } else if (state_ == disabling) {
        state_ = enabled;

        // We tell the people requesting a disable that the disabling
        // process was completed, but that multiple people are using
        // it.
        for (callback_vector_t::iterator p = callbacks_.begin(), e = callbacks_.end(); p < e; ++p) {
            (*p)->multiple_users_seen = true;
            (*p)->on_gc_disabled();
        }

        callbacks_.clear();

        cb->multiple_users_seen = true;
    } else if (state_ == disabled) {
        state_ = enabled;
        cb->multiple_users_seen = false;
    }
    cb->on_gc_enabled();

    return true;
}

void server_t::gc_toggler_t::on_gc_disabled() {
    assert(state_ == disabling);

    int num_serializers = server_->cmd_config->store_dynamic_config.serializer_private.size();

    assert(num_disabled_serializers_ < num_serializers);

    num_disabled_serializers_++;
    if (num_disabled_serializers_ == num_serializers) {
        // We are no longer disabling, we're now disabled!

        bool multiple_disablers_seen = (callbacks_.size() > 1);

        for (callback_vector_t::iterator p = callbacks_.begin(), e = callbacks_.end(); p < e; ++p) {
            (*p)->multiple_users_seen = multiple_disablers_seen;
            (*p)->on_gc_disabled();
        }

        callbacks_.clear();

        state_ = disabled;
    }
}
