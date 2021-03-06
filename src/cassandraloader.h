#pragma once

#include <thread>

#include <cassandra.h>

#include "tile_loader.h"

class CassandraLoader : public TileLoader {
public:
    CassandraLoader(const std::string& contact_points,
                    const std::string& table,
                    std::vector<std::string> versions,
                    uint workers);

    virtual ~CassandraLoader();


    void Load(std::shared_ptr<LoadTask> task, const TileId& tile_id,
              const std::string& version = "") override;

    bool HasVersion(const std::string& version) const override;

    inline bool status() const {
        return connected_;
    }

private:
    static int xy_to_index(int x, int y);

    std::atomic_bool connected_{false};

    CassCluster* cluster_;
    CassSession* session_;
    CassRetryPolicy* retry_policy_;

    std::vector<std::string> versions_;
    std::string table_;
    std::unique_ptr<std::thread> connect_thread_;
};
