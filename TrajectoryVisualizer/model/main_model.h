#ifndef MAINMODEL_H
#define MAINMODEL_H

#include <vector>
#include <exception>

#include "entities/trajectory.h"

namespace modelpkg
{
    class MainModel
    {
    public:
        MainModel();

        //must be used for settings new trajectories,
        //     otherwise  you must manually amended matches
        void                setTrajectory(size_t trj_num, const Trajectory &trj);

                Trajectory& getTrajectory(size_t trj_num);
        const   Trajectory& getTrajectory(size_t trj_num) const;

                Map&        getMainMap()        { return main_map; }
        const   Map&        getMainMap() const  { return main_map; }

                size_t         getTrajectoriesCount() const { return trj_count; }

    class Exception : public std::runtime_error
    {
    public:
        Exception(std::string what)
            : std::runtime_error(what)
        {}
    };

    private:

        Trajectory  first_trj;
        Trajectory  second_trj;
        Map         main_map;

        //from frame from second to the full first_trajectory
        std::vector<std::vector<cv::DMatch>> matches;

        size_t trj_count;
    };
}

#endif // MAINMODEL_H
