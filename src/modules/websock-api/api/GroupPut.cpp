/*
    Copyright (C) 2014-2016 Islog

    This file is part of Leosac.

    Leosac is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Leosac is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "GroupPut.hpp"
#include "Exceptions.hpp"
#include "Group_odb.h"
#include "api/APISession.hpp"
#include "conditions/IsCurrentUserAdmin.hpp"
#include "conditions/IsInGroup.hpp"
#include "core/audit/AuditFactory.hpp"
#include "core/audit/GroupEvent.hpp"
#include "core/auth/Group.hpp"
#include "core/auth/UserGroupMembership.hpp"
#include "core/auth/serializers/GroupJSONSerializer.hpp"
#include "exception/ModelException.hpp"
#include "tools/db/DBService.hpp"

using namespace Leosac;
using namespace Leosac::Module;
using namespace Leosac::Module::WebSockAPI;

GroupPut::GroupPut(RequestContext ctx)
    : MethodHandler(ctx)
{
}

MethodHandlerUPtr GroupPut::create(RequestContext ctx)
{
    auto instance = std::make_unique<GroupPut>(ctx);

    auto has_json_attributes_object = [ptr = instance.get()](const json &req)
    {
        try
        {
            return req.at("attributes").is_object();
        }
        catch (const std::out_of_range &e)
        {
            return false;
        }
    };

    auto will_create = [ptr = instance.get()](const json &req)
    {
        if (req.at("group_id").get<Auth::GroupId>() == 0)
        {
            return true;
        }
        return false;
    };

    auto is_group_admin = [ ctx, ptr = instance.get() ](const json &req)
    {
        auto gid = req.at("group_id").get<Auth::GroupId>();
        if (gid != 0)
        {
            using namespace Conditions;
            return IsInGroup(ctx, gid, Auth::GroupRank::ADMIN)();
        }
        return false;
    };

    // todo check if group admin for update operation.
    // also fix update/create permission

    instance->add_conditions_or(
        []() { throw MalformedMessage("No `attributes` subobject"); },
        has_json_attributes_object);

    instance->add_conditions_or(
        []() { throw PermissionDenied(); }, will_create, is_group_admin,
        Conditions::wrap(Conditions::IsCurrentUserAdmin(ctx)));
    return std::move(instance);
}

json GroupPut::process_impl(const json &req)
{
    auto gid        = req.at("group_id").get<Auth::GroupId>();
    auto attributes = req.at("attributes");

    if (gid == 0)
    {
        return create_group(attributes.at("name"),
                            extract_with_default(attributes, "description", ""));
    }
    else
    {
        return edit_group(gid, attributes.at("name"),
                          extract_with_default(attributes, "description", ""));
    }
}

json GroupPut::create_group(const std::string &name, const std::string &desc)
{
    json rep;
    DBPtr db = ctx_.dbsrv->db();
    odb::transaction t(db->begin());

    // Sanitize input
    Auth::GroupPtr new_group = std::make_shared<Auth::Group>();
    new_group->name(name);
    new_group->description(desc);
    validate_and_unique(new_group);
    db->persist(new_group);

    auto audit = Audit::Factory::GroupEvent(db, new_group, ctx_.audit);
    audit->event_mask(Audit::EventType::GROUP_CREATED);
    audit->after(GroupJSONSerializer::to_string(*new_group));
    audit->finalize();

    // Add the current user to the group as administrator.
    auto audit_add_to_group = Audit::Factory::GroupEvent(db, new_group, ctx_.audit);
    audit_add_to_group->event_mask(Audit::EventType::GROUP_MEMBERSHIP_CHANGED);
    audit_add_to_group->before(GroupJSONSerializer::to_string(*new_group));

    new_group->member_add(ctx_.session->current_user(), Auth::GroupRank::ADMIN);

    db->update(new_group);
    audit_add_to_group->after(GroupJSONSerializer::to_string(*new_group));
    audit_add_to_group->finalize();

    // Send the model back to the client, so it knows the ID.
    rep["data"] = GroupJSONSerializer::to_object(*new_group);
    t.commit();
    return rep;
}

json GroupPut::edit_group(Auth::GroupId id, const std::string &name,
                          const std::string &desc)
{
    json rep;
    DBPtr db = ctx_.dbsrv->db();
    odb::transaction t(db->begin());

    auto grp = db->find<Auth::Group>(id);
    if (!grp)
        throw EntityNotFound(id, "group");

    auto audit = Audit::Factory::GroupEvent(db, grp, ctx_.audit);
    audit->event_mask(Audit::EventType::GROUP_UPDATED);
    grp->name(name);
    grp->description(desc);
    validate_and_unique(grp);
    db->update(grp);

    audit->finalize();
    rep["data"] = GroupJSONSerializer::to_object(*grp);
    t.commit();
    return rep;
}

void GroupPut::validate_and_unique(Auth::GroupPtr grp)
{
    using Query = odb::query<Auth::Group>;

    Auth::GroupValidator::validate(grp);
    auto grp_with_same_name =
        ctx_.dbsrv->db()->query_one<Auth::Group>(Query::name == grp->name());
    // Check that either the name is available, or the group under modification
    // owns the name already.
    if (grp_with_same_name && grp_with_same_name != grp)
    {
        throw ModelException(
            "data/attributes/name",
            BUILD_STR("A group named " << grp->name() << " already exists."));
    }
}
