/***********************************************/
/**
* @file gnssTransmitterGeneratorGnss.cpp
*
* @brief Provides a list of GNSS transmitters.
*
* @author Torsten Mayer-Guerr
* @author Sebastian Strasser
* @date 2021-02-25
*
*/
/***********************************************/

#include "base/import.h"
#include "base/polynomial.h"
#include "inputOutput/logging.h"
#include "files/fileGnssAntennaDefinition.h"
#include "files/fileGnssStationInfo.h"
#include "files/fileGnssSignalBias.h"
#include "files/fileInstrument.h"
#include "files/fileStringTable.h"
#include "gnss/gnssTransmitter.h"
#include "gnss/gnssTransmitterGenerator/gnssTransmitterGeneratorGnss.h"

/***********************************************/

GnssTransmitterGeneratorGnss::GnssTransmitterGeneratorGnss(Config &config)
{
  try
  {
    std::string choice;

    readConfig(config, "inputfileTransmitterList",  fileNamesTransmitterList, Config::MUSTSET,  "{groopsDataDir}/gnss/transmitter/transmitterList.gps.txt", "ascii file with transmitter PRNs, used to loop variable {prn}");
    readConfig(config, "inputfileTransmitterInfo",  fileNameTransmitterInfo,  Config::MUSTSET,  "{groopsDataDir}/gnss/transmitter/transmitterInfo/igs/igs14/transmitterInfo_igs14.{prn}.xml", "variable {prn} available");
    readConfig(config, "inputfileAntennaDefintion", fileNameAntennaDef,       Config::MUSTSET,  "{groopsDataDir}/gnss/transmitter/antennaDefinition/igs/igs14/antennaDefinition_igs14.dat", "phase centers and variations (ANTEX like)");
    if(readConfigChoice(config, "noAntennaPatternFound", choice, Config::MUSTSET, "useNearestFrequency", "what should happen is no antenna pattern is found for an observation"))
    {
      if(readConfigChoiceElement(config, "ignoreObservation",   choice, "ignore observation if no matching pattern is found"))
        noPatternFoundAction = GnssAntennaDefinition::NoPatternFoundAction::IGNORE_OBSERVATION;
      if(readConfigChoiceElement(config, "useNearestFrequency", choice, "use pattern of nearest frequency if no matching pattern is found"))
        noPatternFoundAction = GnssAntennaDefinition::NoPatternFoundAction::USE_NEAREST_FREQUENCY;
      if(readConfigChoiceElement(config, "throwException",      choice, "throw exception if no matching pattern is found"))
        noPatternFoundAction = GnssAntennaDefinition::NoPatternFoundAction::THROW_EXCEPTION;
      endChoice(config);
    }
    readConfig(config, "inputfileSignalDefintion",  fileNameSignalDef,        Config::OPTIONAL, "{groopsDataDir}/gnss/transmitter/signalDefinition/signalDefinition.xml", "transmitted signal types");
    readConfig(config, "inputfileOrbit",            fileNameOrbit,            Config::MUSTSET,  "orbit_{loopTime:%D}.{prn}.dat",    "variable {prn} available");
    readConfig(config, "inputfileAttitude",         fileNameAttitude,         Config::MUSTSET,  "attitude_{loopTime:%D}.{prn}.dat", "variable {prn} available");
    readConfig(config, "inputfileClock",            fileNameClock,            Config::MUSTSET,  "clock_{loopTime:%D}.{prn}.dat",    "variable {prn} available");
    readConfig(config, "interpolationDegree",       interpolationDegree,      Config::DEFAULT,  "7", "for orbit interpolation and velocity calculation");
  }
  catch(std::exception &e)
  {
    GROOPS_RETHROW(e)
  }
}

/***********************************************/

void GnssTransmitterGeneratorGnss::init(const std::vector<Time> &times, std::vector<GnssTransmitterPtr> &transmitters)
{
  try
  {
    logStatus<<"init transmitters"<<Log::endl;

    // read antenna defintion
    std::vector<GnssAntennaDefinitionPtr> antennaDefList;
    readFileGnssAntennaDefinition(fileNameAntennaDef, antennaDefList);

    // read receiver defintion
    std::vector<GnssReceiverDefinitionPtr> signalDefList;
    if(!fileNameSignalDef.empty())
      readFileGnssReceiverDefinition(fileNameSignalDef, signalDefList);

    std::vector<std::string> transmitterList;
    for(const auto &fileNameTransmitterList : fileNamesTransmitterList)
    {
      std::vector<std::string> list;
      readFileStringList(fileNameTransmitterList, list);
      transmitterList.insert(transmitterList.end(), list.begin(), list.end());
    }
    VariableList fileNameVariableList;
    addVariable("prn", fileNameVariableList);
    UInt countTrans = 0;
    for(const std::string &prn : transmitterList)
    {
      try
      {
        fileNameVariableList["prn"]->setValue(prn);

        GnssStationInfo info;
        readFileGnssStationInfo(fileNameTransmitterInfo(fileNameVariableList), info);
        info.fillAntennaPattern(antennaDefList);
        info.fillReceiverDefinition(signalDefList);
        Vector useableEpochs(times.size(), TRUE);

        // read orbit
        // ----------
        std::vector<Time> timesPosVel;
        Matrix            pos, vel;
        try
        {
          const OrbitArc arc = InstrumentFile::read(fileNameOrbit(fileNameVariableList));
          timesPosVel = arc.times();
          const Matrix data = arc.matrix();
          pos = data.column(1, 3);
          vel = data.column(4, 3);
          // check possible data gaps
          Polynomial polynomial(timesPosVel, interpolationDegree, FALSE/*throwException*/);
          const Vector useable = polynomial.interpolate(times, Vector(pos.rows()));
          for(UInt idEpoch=0; idEpoch<useable.rows(); idEpoch++)
            if(std::isnan(useable(idEpoch)))
              useableEpochs(idEpoch) = FALSE;
        }
        catch(std::exception &/*e*/)
        {
          logWarningOnce<<"Unable to read orbit file <"<<fileNameOrbit(fileNameVariableList)<<">, disabling transmitter."<<Log::endl;
          continue;
        }

        // read attitude
        // -------------
        std::vector<Transform3d> crf2srf(times.size());
        try
        {
          const StarCameraArc arc = InstrumentFile::read(fileNameAttitude(fileNameVariableList));
          Polynomial polynomial(arc.times(), interpolationDegree, FALSE/*throwException*/);
          const Matrix quaternion = polynomial.interpolate(times, arc.matrix().column(1, 4));
          for(UInt idEpoch=0; idEpoch<quaternion.rows(); idEpoch++)
            if(!std::isnan(quaternion(idEpoch,0)))
              crf2srf.at(idEpoch) = inverse(Rotary3d(quaternion.row(idEpoch).trans()/norm(quaternion.row(idEpoch))));
            else
              useableEpochs(idEpoch) = FALSE;
        }
        catch(std::exception &/*e*/)
        {
          logWarningOnce<<"Unable to read attitude file <"<<fileNameAttitude(fileNameVariableList)<<">, disabling transmitter."<<Log::endl;
          continue;
        }

        // satellite clocks
        // ----------------
        std::vector<Double> clock;
        try
        {
          const MiscValueArc arc = InstrumentFile::read(fileNameClock(fileNameVariableList));
          Polynomial polynomial(arc.times(), 1, FALSE/*throwException*/); // linear interpolation
          clock = Vector(polynomial.interpolate(times, arc.matrix().column(1)));
          for(UInt idEpoch=0; idEpoch<clock.size(); idEpoch++)
            if(std::isnan(clock.at(idEpoch)))
              useableEpochs(idEpoch) = FALSE;
        }
        catch(std::exception &/*e*/)
        {
          logWarningOnce<<"Unable to read clock file <"<<fileNameClock(fileNameVariableList)<<">, disabling transmitter."<<Log::endl;
          continue;
        }

        // test completeness of antennas
        for(const auto &antenna : info.antenna)
          if(antenna.timeEnd >= times.front() && antenna.timeStart < times.back() && !antenna.antennaDef)
            logWarningOnce<<info.markerName<<"."<<info.markerNumber<<": No antenna definition found for "<<antenna.str()<<Log::endl;

        std::vector<Vector3d>    offset(times.size());
        std::vector<Transform3d> srf2arf(times.size());
        for(UInt idEpoch=0; idEpoch<times.size(); idEpoch++)
          if(useableEpochs(idEpoch))
          {
            const UInt idAnt = info.findAntenna(times.at(idEpoch));
            if(idAnt != NULLINDEX && info.antenna.at(idAnt).antennaDef)
            {
              offset.at(idEpoch)  = info.antenna.at(idAnt).position - info.referencePoint(times.at(idEpoch));
              srf2arf.at(idEpoch) = info.antenna.at(idAnt).local2antennaFrame;
            }
            else
              useableEpochs(idEpoch) = FALSE;
          }

        // test useable
        // ------------
        const UInt countUseableEpochs = static_cast<UInt>(sum(useableEpochs));
        if(!countUseableEpochs)
        {
          logWarningOnce<<info.markerName<<"."<<info.markerNumber<<": No usable epochs, disabling transmitter."<<Log::endl;
          continue;
        }
        if(countUseableEpochs < useableEpochs.rows())
          logWarningOnce<<info.markerName<<"."<<info.markerNumber<<": "<<useableEpochs.rows()-countUseableEpochs<<" epochs disabled due to missing orbit/attitude/clock data"<<Log::endl;

        transmitters.push_back(std::make_shared<GnssTransmitter>(GnssType("***"+info.markerNumber), prn, info, noPatternFoundAction,
                                                                 useableEpochs, clock, offset, crf2srf, srf2arf, timesPosVel, pos, vel, interpolationDegree));
        countTrans++;
      }
      catch(std::exception &e)
      {
        logWarningOnce<<"Unable to initialize satellite: "<<e.what()<<Log::endl;
      }
    } // for(idTrans)

    if(!countTrans)
    {
      fileNameVariableList["prn"]->setValue("***");
      logWarningOnce<<"Initialization of all satellites failed. Wrong file name <"<<fileNameOrbit(fileNameVariableList)<<">?"<<Log::endl;
    }
  }
  catch(std::exception &e)
  {
    GROOPS_RETHROW(e)
  }
}

/***********************************************/
